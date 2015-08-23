#include <iostream>
#include <map>
#include <set>
#include <stdlib.h>
#include <string.h>
#include <boost/thread.hpp>
#include "Channel.h"
#include "MessageLog.h"
#include "MessageEncoding.h"
#include "value.h"
#include "MessagingInterface.h"
#include "SocketMonitor.h"
#include "ConnectionManager.h"
#include "Logger.h"
#include "ClientInterface.h"
#include "IODCommand.h"
#include "IODCommands.h"
#include "SyncRemoteStatesAction.h"
#include "DebugExtra.h"

std::map<std::string, Channel*> *Channel::all = 0;
std::map< std::string, ChannelDefinition* > *ChannelDefinition::all = 0;

boost::mutex Channel::update_mutex;

State ChannelImplementation::CONNECTING("CONNECTING");
State ChannelImplementation::DISCONNECTED("DISCONNECTED");
State ChannelImplementation::DOWNLOADING("DOWNLOADING");
State ChannelImplementation::WAITSTART("WAITSTART");
State ChannelImplementation::UPLOADING("UPLOADING");
State ChannelImplementation::CONNECTED("CONNECTED");
State ChannelImplementation::ACTIVE("ACTIVE");

class ChannelInternals {
public:
	boost::mutex iod_cmd_get_mutex;
	boost::mutex iod_cmd_put_mutex;
	std::list<IODCommand*> pending_commands;
	std::list<IODCommand*> completed_commands;
};

class chn_scoped_lock {
public:
	chn_scoped_lock( const char *loc, boost::mutex &mut) : location(loc), mutex(mut), lock(mutex, boost::defer_lock) { 
//		NB_MSG << location << " LOCKING...\n";
		lock.lock(); 
//		NB_MSG << location << " LOCKED...\n";
	}
	~chn_scoped_lock() { 
		lock.unlock(); 
//		NB_MSG << location << " UNLOCKED\n";
		}
	std::string location;
	boost::mutex &mutex;
	boost::unique_lock<boost::mutex> lock;
};



MachineRef::MachineRef() : refs(1) {
}

MachineRecord::MachineRecord(MachineInstance *m) : machine(m), last_sent(0) {
}

ChannelImplementation::ChannelImplementation() : monitors_exports(false), authority(0) {
	authority = random();
}

ChannelImplementation::~ChannelImplementation() { }


Channel::Channel(const std::string ch_name, const std::string type)
        : MachineInstance(ch_name.c_str(), type.c_str()), ChannelImplementation(),
            internals(0), name(ch_name), port(0), mif(0), communications_manager(0),
			monit_subs(0), monit_pubs(0),
			connect_responder(0), disconnect_responder(0),
			monitor_thread(0),
			throttle_time(0), connections(0), aborted(false), subscriber_thread(0),
			cmd_client(0), cmd_server(0)
{
	internals = new ChannelInternals();
    if (all == 0) {
        all = new std::map<std::string, Channel*>;
    }
    (*all)[name] = this;
	if (::machines.find(ch_name) == ::machines.end())
		::machines[ch_name] = this;
	markActive(); // this machine performs state changes in the ::idle() processing
}

Channel::~Channel() {
    stop();
    std::set<MachineInstance*>::iterator iter = this->channel_machines.begin();
    while (iter != channel_machines.end()) {
        MachineInstance *machine = *iter++;
        machine->unpublish();
    }
	::machines.erase(_name);
    this->channel_machines.clear();
	busy_machines.erase(this);
	all_machines.remove(this);
	pending_state_change.erase(this);
    remove(name);
}

void Channel::syncInterfaceProperties(MachineInstance *m) {
	if (!definition()->hasFeature(ChannelDefinition::ReportPropertyChanges)) return;
	if (m->isShadow()) return;
	if ( definition()->updates_names.count(m->getName())
		|| definition()-> shares_names.count(m->getName())) {

		std::map<std::string, Value>::const_iterator found = definition()->updates_names.find(m->getName());
		if (found == definition()->updates_names.end()) {
			found = definition()->shares_names.find(m->getName());
			if (found == definition()->shares_names.end()) return;
		}
		const std::pair<std::string, Value>elem = *found;
		std::string interface_name(elem.second.asString());

		MachineClass *mc = MachineClass::find(interface_name.c_str());
		if (!mc) {
			DBG_CHANNELS << "Warning: Interface " << interface_name << " is not defined\n";
			return;
		}
		MachineInterface *mi = dynamic_cast<MachineInterface*>(mc);
		if (mi) {
			DBG_CHANNELS << "sending properties for machine " << m->getName() <<":" << mi->name << "\n";
			std::set<std::string>::iterator props = mi->property_names.begin();
			while (props != mi->property_names.end()) {
				const std::string &s = *props++;
				Value v = m->getValue(s);
				if (v != SymbolTable::Null) {
					char *cmd = MessageEncoding::encodeCommand("PROPERTY", m->getName(), s, v);
					std::string response;
					// TBD this can take some time. need to remember where we are up to and come back later
					sendMessage(cmd, *cmd_client, response);
					//safeSend(*cmd_client, cmd, strlen(cmd));
				}
				else {
					DBG_CHANNELS << "Note: machine " << m->getName() << " does not have a property "
						<< s << " corresponding to interface " << interface_name << "\n";
				}
			}
		}
		else {
			DBG_CHANNELS << "could not find the interface definition for " << m->getName() << "\n";
		}
	}
}

bool Channel::syncRemoteStates() {
	//DBG_CHANNELS << "Channel " << name << " syncRemoteStates " << current_state << "\n";
	// publishers do not initially send the state of all machines
	if (definition()->isPublisher()) return false;
	if (current_state == ChannelImplementation::DISCONNECTED) return false;
	if (isClient()) {
		if (definition()->hasFeature(ChannelDefinition::ReportStateChanges)) {
			std::set<MachineInstance*>::iterator iter = this->channel_machines.begin();
			while (iter != channel_machines.end()) {
				MachineInstance *m = *iter++;
				if (!m->isShadow()) {
					std::string state(m->getCurrentStateString());
					DBG_CHANNELS << "Machine " << m->getName() << " current state: " << state << "\n";
					char *msg = MessageEncoding::encodeState(m->getName(), state, authority);
					std::string response;
					// TBD this can take some time. need to remember where we are up to and come back later
					sendMessage(msg, *cmd_client, response);
					//safeSend(*cmd_client, msg, strlen(msg));
					free(msg);
				}
			}
		}
		if (definition()->hasFeature(ChannelDefinition::ReportPropertyChanges)) {
			std::set<MachineInstance*>::iterator iter = this->channel_machines.begin();
			while (iter != channel_machines.end()) {
				MachineInstance *m = *iter++;
				// look at the interface defined for this machine and for each property
				// on the interface, send the current value
				syncInterfaceProperties(m);
			}
		}
		//safeSend(*cmd_client, "done", 4);
		std::string ack;
		sendMessage("done", *cmd_client, ack);
		DBG_CHANNELS << "channel " << name << " got " << ack << " from server\n";
		SetStateActionTemplate ssat(CStringHolder("SELF"), "ACTIVE" );
		SetStateAction *ssa = (SetStateAction*)ssat.factory(this);
		// execute this state change once all other actions are
		enqueueAction(ssa);
	}
	else {
		if (definition()->hasFeature(ChannelDefinition::ReportStateChanges)) {
			std::set<MachineInstance*>::iterator iter = this->channel_machines.begin();
			while (iter != channel_machines.end()) {
				MachineInstance *m = *iter++;
				if (!m->isShadow()) {
					const char *state = m->getCurrentStateString();
					char buf[200];
					if (cmd_client) {
						const char *msg = MessageEncoding::encodeState(m->getName(), state, definition()->authority);
						std::string response;
						sendMessage(msg, *cmd_client, response);
						//safeSend(*cmd_client, msg, strlen(msg));
					}
					else
						sendStateChange(m, state, definition()->authority);
				}
			}
		}
		if (definition()->hasFeature(ChannelDefinition::ReportPropertyChanges)) {
			std::set<MachineInstance*>::iterator iter = this->channel_machines.begin();
			while (iter != channel_machines.end()) {
				MachineInstance *m = *iter++;
				// look at the interface defined for this machine and for each property
				// on the interface, send the current value
				syncInterfaceProperties(m);
			}
		}
		SetStateActionTemplate ssat(CStringHolder("SELF"), "DOWNLOADING" );
		enqueueAction(ssat.factory(this)); // execute this state change once all other actions are
/*
 		if (cmd_client) {
			std::string ack;
			sendMessage("done", *cmd_client, ack);
		}
		else {
			assert(false);
			mif->send("done");
		}
*/
	}
	//DBG_CHANNELS << "Channel " << name << " syncRemoteStatesDone\n";
	return true;
}

Action::Status Channel::setState(const State &new_state, uint64_t authority, bool resume) {
	if (new_state != ChannelImplementation::DISCONNECTED && connections == 0) {
		// can only change state if the channel is actually connected
		if (!communications_manager || (isClient() && !communications_manager->monit_setup) ) {
			{
				FileLogger fl(program_name);
				fl.f() << name << " state change to "
					<< new_state << " failed. Subscription manager is not initialised\n";
			}
			return Action::Failed;
		}
		if ( ( isClient() && communications_manager->monit_setup->disconnected() )
			|| communications_manager->monit_subs.disconnected() ) {
				{FileLogger fl(program_name);
				fl.f() << name << " state chanage to " << new_state << " failed. command socket is not connected\n"; }
				return Action::Failed;
		}
		if ( communications_manager->monit_subs.disconnected() ) {
				{FileLogger fl(program_name);
				fl.f() << name << " state chanage to " << new_state << " failed. Subscriber is disconnected\n"; }
				return Action::Failed;
		}
	}

	Action::Status res = MachineInstance::setState(new_state, resume);

	if (res != Action::Complete) {
		DBG_CHANNELS << "Action " << *this << " not complete\n";
		return res;
	}
	if (new_state == ChannelImplementation::CONNECTED) {
		DBG_CHANNELS << name << " CONNECTED\n";
		enableShadows();
		setNeedsCheck();
		if (isClient()) {
			SetStateActionTemplate ssat(CStringHolder("SELF"), "DOWNLOADING" );
			enqueueAction(ssat.factory(this)); // execute this state change once all other actions are
		}
	}
	else if (new_state == ChannelImplementation::WAITSTART) {
		DBG_CHANNELS << name << " WAITSTART\n";
		enableShadows();
	}
	else if (new_state == ChannelImplementation::UPLOADING) {
		DBG_CHANNELS << name << " UPLOADING\n";
		setNeedsCheck();
		SyncRemoteStatesActionTemplate srsat;
		enqueueAction(srsat.factory(this));
	}
	else if (new_state == ChannelImplementation::DOWNLOADING) {
		DBG_CHANNELS << name << " DOWNLOADING\n";
		std::string ack;
		if (isClient()) {
			sendMessage("status", *cmd_client, ack);
			DBG_CHANNELS << "channel " << name << " got ack: " << ack << " to start request\n";
			//safeSend(*cmd_client, "status", 6);
		}
		else {
			sendMessage("done", *cmd_client, ack);
			DBG_CHANNELS << "channel " << name << " got ack: " << ack << " when finished upload\n";
			//safeSend(*cmd_client, "done", 4);
		}
	}
	else if (new_state == ChannelImplementation::DISCONNECTED) {
		DBG_CHANNELS << name << " DISCONNECTED\n";
		disableShadows();
		setNeedsCheck();
	}
	return res;
}

Action::Status Channel::setState(const char *new_state_cstr, uint64_t authority, bool resume) {
	State new_state(new_state_cstr);
	return setState(new_state, resume);
}

void Channel::start() {
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);
	DBG_CHANNELS << "Channel " << name << " starting from thread "<< tnam << "\n";


	if (isClient()) {
		startSubscriber();
	}
	else {
		if (definition()->isPublisher())
			startServer(eZMQ);
		else
			startSubscriber();
	}
	started_ = true;
}

void Channel::stop() {
	//TBD fix machines that publish to this channel
    disableShadows();
	if (isClient()) {
		if (definition()->monitors() || monitors()) stopSubscriber();
	}
	else {
		if (definition()->updates() || updates()
		|| definition()->shares_machines() || shares_machines())
			stopSubscriber();
	}
	started_ = false;
}

void Channel::startChannels() {
	if (!all) return;
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        Channel *chn = (*iter).second; iter++;
		if (!chn->started()) chn->start();
	}
}

//NOTE: currently this method is not used
void Channel::stopChannels() {
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        Channel *chn = (*iter).second; iter++;
		if (!chn->started()) chn->stop();
	}
}


bool Channel::started() { return started_; }

void Channel::addConnection() {
	boost::mutex::scoped_lock lock(update_mutex);
	++connections;
	{FileLogger fl(program_name);
	fl.f() << getName() << " client number " << connections << " connected in state " << current_state << "\n";}

	if (connections == 1) {
		if (isClient()){
			if (definition()->isPublisher()) {
				SetStateActionTemplate ssat(CStringHolder("SELF"), "ACTIVE" );
				enqueueAction(ssat.factory(this)); // execute this state change once all other actions are complete
			}
			else {
				SetStateActionTemplate ssat_connected(CStringHolder("SELF"), "CONNECTED" );
				enqueueAction(ssat_connected.factory(this)); // execute this state change once all other actions are complete
				SetStateActionTemplate ssat(CStringHolder("SELF"), "DOWNLOADING" );
				enqueueAction(ssat.factory(this)); // execute this state change once all other actions are complete
			}
		}
		else {
			if (definition()->isPublisher()) {
				SetStateActionTemplate ssat(CStringHolder("SELF"), "ACTIVE" );
				enqueueAction(ssat.factory(this)); // execute this state change once all other actions are complete
			}
			else {
				SetStateActionTemplate ssat(CStringHolder("SELF"), "WAITSTART" );
				enqueueAction(ssat.factory(this)); // execute this state change once all other actions are complete
			}
		}
	}
	else if (isClient())
		assert(false);
	else if (!definition()->isPublisher()) {
		if (current_state != ChannelImplementation::WAITSTART) {
			DBG_CHANNELS << "Channel " << getName() << " waiting for start\n";
			SetStateActionTemplate ssat(CStringHolder("SELF"), "WAITSTART" );
			enqueueAction(ssat.factory(this)); // execute this state change once all other actions are complete
		}
	}
 }

void Channel::dropConnection() {
	boost::mutex::scoped_lock lock(update_mutex);
	assert(connections);
	--connections;
	{FileLogger fl(program_name); fl.f() << getName() << " client disconnected\n"; }
	if(!connections) {
		SetStateActionTemplate ssat(CStringHolder("SELF"), "DISCONNECTED" );
		enqueueAction(ssat.factory(this)); // execute this state change once all other actions are complete
	}
	assert(connections>=0);
	if (connections == 0) {
		//DBG_CHANNELS << "last connection dropped, stopping channel " << _name << "\n";
		//stopServer();
		//delete this;
	}
}


Channel &Channel::operator=(const Channel &other) {
    assert(false);
    return *this;
}

std::ostream &Channel::operator<<(std::ostream &out) const  {
    out << "CHANNEL";
    return out;
}

std::ostream &operator<<(std::ostream &out, const Channel &m) {
    return m.operator<<(out);
}

bool Channel::operator==(const Channel &other) {
    return false;
}


void Channel::setPort(unsigned int new_port) {
    assert(port == 0);
    port = new_port;
}
unsigned int Channel::getPort() const { return port; }


int Channel::uniquePort(unsigned int start, unsigned int end) {
    int res = 0;
    char address_buf[40];
    while (true) {
        try{
            zmq::socket_t test_bind(*MessagingInterface::getContext(), ZMQ_PULL);
            res = random() % (end-start+1) + start;
            snprintf(address_buf, 40, "tcp://*:%d", res);
            test_bind.bind(address_buf);
            int linger = 0; // do not wait at socket close time
            test_bind.setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
            //DBG_CHANNELS << "found available port " << res << "\n";
            break;
        }
        catch (zmq::error_t err) {
            if (zmq_errno() != EADDRINUSE) {
                break;
            }
        }
    }
    return res;
}

ChannelDefinition::ChannelDefinition(const char *n, ChannelDefinition *prnt)
		: MachineClass(n),parent(prnt), ignore_response(false) {
    if (!all) all = new std::map< std::string, ChannelDefinition* >;
    (*all)[n] = this;
	states.push_back("DISCONNECTED");
	states.push_back("CONNECTED");
	states.push_back("CONNECTING");
	states.push_back("WAITSTART");
	states.push_back("UPLOADING");
	states.push_back("DOWNLOADING");
	states.push_back("ACTIVE");
	default_state = State("DISCONNECTED");
	initial_state = State("DISCONNECTED");
	features.insert(ReportPropertyChanges);
	features.insert(ReportStateChanges);
	disableAutomaticStateChanges();
}

void ChannelDefinition::addFeature(Feature f) {
	features.insert(f);
}

void ChannelDefinition::removeFeature(Feature f) {
	features.erase(f);
}

bool ChannelDefinition::hasFeature(Feature f) const {
	return features.count(f) != 0;
}

ChannelDefinition *ChannelDefinition::find(const char *name) {
    if (!all) return 0;
    std::map< std::string, ChannelDefinition* >::iterator found = all->find(name);
    if (found == all->end()) return 0;
    return (*found).second;
}
Channel *ChannelDefinition::instantiate(unsigned int port) {
    
    Channel *chn = Channel::findByType(name);
    if (!chn) chn = Channel::create(port, this);
    return chn;
}

Value ChannelDefinition::getValue(const char *property) {
	std::map<std::string, Value>::iterator iter = options.find(property);
	if (iter == options.end()) return SymbolTable::Null;
	return (*iter).second;
}

bool ChannelDefinition::isPublisher() const { return ignore_response; }
void ChannelDefinition::setPublisher(bool which) { ignore_response = which; }

bool ChannelImplementation::monitors() const {
	return monitors_exports
		|| !monitors_names.empty()
		|| !monitors_patterns.empty()
		|| !monitors_properties.empty();
}

bool ChannelImplementation::updates() const {
	return !updates_names.empty();
}

bool ChannelImplementation::shares_machines() const {
	return !shares_names.empty();
}


class ChannelConnectMonitor : public EventResponder {
public:
	ChannelConnectMonitor(Channel *channel) : chn(channel) {}
	void operator()(const zmq_event_t &event_, const char* addr_) {
		chn->addConnection();
	}
	Channel *chn;
};
class ChannelDisconnectMonitor : public EventResponder {
public:
	ChannelDisconnectMonitor(Channel *channel) : chn(channel) {}
	void operator()(const zmq_event_t &event_, const char* addr_) {
		chn->dropConnection();
	}
	Channel *chn;
};

void Channel::startServer(ProtocolType proto) {
	//DBG_CHANNELS << name << " startServer\n";
	if (mif) {
		DBG_CHANNELS << "Channel::startServer() called when mif is already allocated\n";
		return;
	}
	if (monitor_thread) {
		DBG_CHANNELS << "Channel " << name << " already prepared as a server\n";
		return;
	}

	std::string socknam("inproc://");
	mif = MessagingInterface::create("*", port, proto);
	monit_subs = new SocketMonitor(*mif->getSocket(), mif->getURL().c_str());

	connect_responder = new ChannelConnectMonitor(this);
	disconnect_responder = new ChannelDisconnectMonitor(this);
	monit_subs->addResponder(ZMQ_EVENT_ACCEPTED, connect_responder);
	monit_subs->addResponder(ZMQ_EVENT_DISCONNECTED, disconnect_responder);
	monitor_thread = new boost::thread(boost::ref(*monit_subs));
	mif->start();
}

void Channel::startClient() {
	DBG_CHANNELS << name << " startClient\n";
	if (mif) {
		DBG_CHANNELS << "Channel::startClient() called when mif is already allocated\n";
		return;
	}
	std::string socknam("inproc://");
	Value host = getValue("host");
	if (host == SymbolTable::Null)
		host = "localhost";
	Value port_val = getValue("port");
	if (port_val == SymbolTable::Null) port = 5555; else {
		long remote_port;
		if (port_val.asInteger(remote_port)) port = (int)remote_port; else port = 5555;
	}

	mif = MessagingInterface::create(host.asString().c_str(), port, eCHANNEL);
	monit_subs = new SocketMonitor(*mif->getSocket(), mif->getURL().c_str());

	connect_responder = new ChannelConnectMonitor(this);
	disconnect_responder = new ChannelDisconnectMonitor(this);
	//cmd_socket = &subscription_manager.setup;
	monit_subs->addResponder(ZMQ_EVENT_ACCEPTED, connect_responder);
	monit_subs->addResponder(ZMQ_EVENT_DISCONNECTED, disconnect_responder);
	monitor_thread = new boost::thread(boost::ref(*monit_subs));
	mif->start();
}

// intended to be called when a connection is dropped before the
// connection is deleted but we no longer delete connections
//
void Channel::stopServer() {
	return;

	if (monitor_thread) {
		monit_subs->abort();
		delete monitor_thread;
		monitor_thread = 0;
	}
	if (monit_subs) {
		if (connect_responder) {
			monit_subs->removeResponder(ZMQ_EVENT_ACCEPTED, connect_responder);
			delete connect_responder;
			connect_responder = 0;
		}
		if (disconnect_responder) {
			monit_subs->removeResponder(ZMQ_EVENT_DISCONNECTED, disconnect_responder);
			delete disconnect_responder;
			disconnect_responder = 0;
		}
		delete monit_subs;
		monit_subs = 0;
	}
}

// the client and server enter the downloading and uploading states
// in opposite orders.

// This is used within the channels thread to manage the initial exchange
// of data for the channel

void Channel::checkStateChange(std::string event) {
	DBG_CHANNELS << "Received " << event << " in " << current_state << " on " << name << "\n";
	
	if (current_state == ChannelImplementation::DISCONNECTED) {
		checkCommunications();
	}
	if (current_state == ChannelImplementation::DISCONNECTED) {
		return;
	}
	if (isClient()) {
		if ( current_state == ChannelImplementation::DOWNLOADING ) {
			if (setState(ChannelImplementation::UPLOADING) == Action::Failed) {
			}
		}
		else if (current_state == ChannelImplementation::UPLOADING) {
			DBG_CHANNELS << name << " -> ACTIVE\n";
			setState(ChannelImplementation::ACTIVE);
		}
		else {
			DBG_CHANNELS << "Unexpected channel state " << current_state << " on " << name << "\n";
			//assert(false);
		}
	}
	else {
		// note that the start event may arrive before our switch to waitstart.
		// we rely on the client resending if necessary
		if ( current_state == ChannelImplementation::CONNECTED && event == "status" )
			setState(ChannelImplementation::UPLOADING);
		if ( current_state == ChannelImplementation::WAITSTART && event == "status" )
			setState(ChannelImplementation::UPLOADING);
		else if (current_state == ChannelImplementation::ACTIVE && event == "status") {
			{FileLogger fl(program_name); fl.f() << "ignoring " << event << " while active\n"; }
			//setState(ChannelImplementation::UPLOADING);
		}
		else if (current_state == ChannelImplementation::UPLOADING)
			setState(ChannelImplementation::ACTIVE);
		else if (current_state == ChannelImplementation::DOWNLOADING)
			setState(ChannelImplementation::ACTIVE);
		else {
			DBG_CHANNELS << "Unexpected channel state " << current_state << " on " << name << "\n";
			//assert(false);
		}
	}
}

bool Channel::isClient() {
	return (definition_file != "dynamic");
}

// used to stop the channel thread
void Channel::abort() { aborted = true; }

void Channel::operator()() {
	std::string thread_name(name);
	thread_name += " subscriber";
#ifdef __APPLE__
	pthread_setname_np(thread_name.c_str());
#else
	pthread_setname_np(pthread_self(), thread_name.c_str());
#endif

	/* Clients need to use a Subscription Manager to connect to the remote end
	 	and need to setup a host and port for that purpose.
	 	Servers need to monitor a subscriber socket
	 	Both cases use a cmd socket to communicate between the main thread and
	 	the Subscription Manager.
	 */
	if (isClient()){
		Value host = getValue("host");
		Value port_val = getValue("port");
		if (host == SymbolTable::Null)
			host = "localhost";
		long port = 0;
		if (!port_val.asInteger(port)) return;

		DBG_CHANNELS << " channel " << _name << " starting subscription to " << host << ":" << port << "\n";
		communications_manager = new SubscriptionManager(definition()->name.c_str(),
				eCHANNEL, host.asString().c_str(),(int)port);
	}
	else {
		communications_manager = new SubscriptionManager(definition()->name.c_str(), eCHANNEL, "*", port);
	}

	{
		int retry = 1;
		// TBD why is it necessary to have this loop? may introduce more problems than it solves
		while (cmd_server == 0)  {
			try {
				cmd_server = createCommandSocket(false);
				if (!cmd_server) {
					DBG_CHANNELS << "failed to create internal channel command listener socket\n";
					if (--retry == 0) { assert(false); exit(2); }
					usleep(10);
				}
			}
			catch(zmq::error_t err) {
				DBG_CHANNELS << "Channel " << name << " ZMQ error: " << zmq_strerror(errno)
					<< " trying to create internal channel command listener socket\n";
				if (--retry == 0) { assert(false); exit(2); }
				usleep(10);
			}
		}
	}
	usleep(500);
	char start_cmd[20];
	DBG_CHANNELS << "channel " << name << " thread waiting for start message\n";
	size_t start_len;
	safeRecv(*cmd_server, start_cmd, 20, true, start_len, 0);
	if (!start_len) {
		FileLogger fl(program_name);
		fl.f() << name << " error getting start message\n";
	}
	safeSend(*cmd_server, "ok", 2);

	zmq::pollitem_t *items = 0;
	DBG_CHANNELS << "channel " << name << " thread received start message\n";

	//SetStateActionTemplate ssat(CStringHolder("SELF"), "CONNECTED" );
	//enqueueAction(ssat.factory(this)); // execute this state change once all other actions are

	SubscriptionManager *sm = dynamic_cast<SubscriptionManager*>(communications_manager);
	int cmd_server_idx = 0;
	try {
		while (!aborted && communications_manager) {
			// clients have a socket to setup the channel, servers do not need it
			int num_poll_items = 0;
			int subscriber_idx = 0;
			{
				if (communications_manager->isClient()) {
					num_poll_items = 3;
					subscriber_idx = 1;
				}
				else {
					num_poll_items = 2;
					subscriber_idx = 0;
				}
				if (items) delete[] items;
				items = new zmq::pollitem_t[num_poll_items];
				int idx = communications_manager->configurePoll(items);
				assert(idx < num_poll_items);
				items[idx].socket = *cmd_server;
				items[idx].events = ZMQ_POLLERR | ZMQ_POLLIN;
				items[idx].fd = 0;
				items[idx].revents = 0;
				cmd_server_idx = idx;
			}
			bool no_commands;
			{
				chn_scoped_lock ("main loop checking for completed command", internals->iod_cmd_put_mutex);
				no_commands = internals->completed_commands.empty();
			}
			try {
				if (no_commands
					&& !communications_manager->checkConnections(items, num_poll_items, *cmd_server)) {
					usleep(1000);
					continue;
				}
			}
			catch(zmq::error_t err) {
				DBG_CHANNELS << "Channel " << name << " ZMQ error: " << zmq_strerror(errno) << "\n";
				continue;
			}
			{
				boost::unique_lock<boost::mutex> lock1(internals->iod_cmd_put_mutex, boost::defer_lock);
				//boost::unique_lock<boost::mutex> lock2(internals->iod_cmd_get_mutex, boost::defer_lock);
				//NB_MSG << "main loop trying completed lock for cleaning up\n";
				if (lock1.try_lock()) {
					//NB_MSG << "main loop got completed lock for cleaning up\n";
					//chn_scoped_lock ("main loop cleaning completed", internals->iod_cmd_put_mutex);
					if (!internals->completed_commands.empty()) {
						std::string response;
						DBG_CHANNELS << "Channel " << name << " finishing off processed commands\n";
						std::list<IODCommand*>::iterator iter = internals->completed_commands.begin();
						while (iter != internals->completed_commands.end()) {
							IODCommand *cmd = *iter;
							iter = internals->completed_commands.erase(iter);
							if (cmd->done == IODCommand::Success) {
								DBG_CHANNELS << cmd->param(0) <<" succeeded: " << cmd->result() << "\n";
							}
							else {
								DBG_CHANNELS << cmd->param(0) <<" failed: " << cmd->error() << "\n";
							}
							delete cmd;
						}
						// TBD should this go back to the originator?
					}
					//NB_MSG << "main loop cleaned up completed. unlocking.\n";
					lock1.unlock();
					//internals->iod_cmd_put_mutex.unlock();
				}

				if (isClient()
						&& !sm->monit_subs.disconnected()
						&& sm->subscriberStatus() == SubscriptionManager::ss_ready
						&& current_state == ChannelImplementation::DOWNLOADING) {

					long current_time = getTimerVal()->iValue;
					if (current_time > 1000) {
						if (current_time > 2000) {
							FileLogger fl(program_name);
							fl.f() << "Channel " << name << " state change has not occurred after " << current_time/1000 << "s\n";
							if (current_time > 10000) {
								fl.f() << name << " aborting\n";
								exit(53);
							}
						}
						else {
							DBG_CHANNELS << "channel " << name << " switching to CONNECTED state\n";
							SetStateActionTemplate ssat(CStringHolder("SELF"), "CONNECTED" );
							enqueueAction(ssat.factory(this)); // execute this state change once all other actions are
						}
					}
				}

				if (items[cmd_server_idx].revents & ZMQ_POLLIN) {
					DBG_CHANNELS << name << " incoming activity on channel\n";
				}

				if (items[subscriber_idx].revents & ZMQ_POLLERR) {
					{FileLogger fl(program_name);
						fl.f() << name << " thread detected error on subscriber connection\n"; }
				}

				if ( !(items[subscriber_idx].revents & ZMQ_POLLIN)
						|| (items[subscriber_idx].revents & ZMQ_POLLERR) )
					continue;

				// incoming channel data
				char *data;
				size_t len;
				safeRecv(communications_manager->subscriber(), &data, &len, false, 0);
				DBG_CHANNELS << "Channel " << name << " subscriber received: " << data << " len:" << len << "\n";
				if (len == 0) continue;
				if (strncmp(data, "done", len) == 0 || strncmp(data, "status", len) == 0) {
					checkStateChange(data);
				}
				else
				{
					IODCommand *command = parseCommandString(data);
					if (command) {
						chn_scoped_lock lock("main loop add pending command", internals->iod_cmd_get_mutex);
						internals->pending_commands.push_back(command);
					}
				}
				delete[] data;
			}
		}
	}
	catch (std::exception ex) {
		NB_MSG << "Channel " << name << " saw exception " << ex.what() << "\n";
	}
}

void Channel::sendMessage(const char *msg, zmq::socket_t &sock) {
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);
	if (!subscriber_thread) {
		//return ::sendMessage(msg, sock);
		safeSend(sock, msg, strlen(msg));
	}
	else {
		safeSend(*cmd_client, msg, strlen(msg));
	}
}

bool Channel::sendMessage(const char *msg, zmq::socket_t &sock, std::string &response) {
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);

	if (!subscriber_thread) {
		DBG_CHANNELS << tnam << " Channel " << name << " sendMessage() sending " << msg << " directly\n";
		return ::sendMessage(msg, sock, response);
		/*safeSend(sock, msg, strlen(msg));
		usleep(10);
		zmq::message_t resp;
		char buf[200];
		size_t len;
		safeRecv(sock, buf, 200, true, len);*/
	}
	else {
		DBG_CHANNELS << "Channel " << name << " sendMessage() sending " << msg << " through a channel thread\n";
		safeSend(*cmd_client, msg, strlen(msg));
		char *response_buf;
		size_t rlen;
		safeRecv(*cmd_client, &response_buf, &rlen, true, 0);
		response = response_buf;
		DBG_CHANNELS << "Channel " << name << " sendMessage() got response for " << msg << " from a channel thread\n";

	}
	return true;
}

zmq::socket_t *Channel::createCommandSocket(bool client_endpoint) {
	char cmd_socket_name[100];
	snprintf(cmd_socket_name, 100, "inproc://%s_cmd", name.c_str());
	DBG_CHANNELS << "using " << cmd_socket_name
		<< " for the " << ( (client_endpoint) ? "client " : "server ") << "command socket\n";
	char *pos = strchr(cmd_socket_name, ':')+1;
	while ( (pos = strchr(pos, ':'))  ) *pos = '-';

	if (client_endpoint) {
		zmq::socket_t *sock = new zmq::socket_t(*MessagingInterface::getContext(), ZMQ_REQ);
		sock->connect(cmd_socket_name);
		DBG_CHANNELS << name << " connected channel command client\n";
		return sock;
	}
	else {
		zmq::socket_t *sock = new zmq::socket_t(*MessagingInterface::getContext(), ZMQ_REP);
		try {
			sock->bind(cmd_socket_name);
			int linger = 0;
			sock->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
			DBG_CHANNELS << name << " bound channel command server\n";
			return sock;
		}
		catch(std::exception ex) {
			assert(false);
			return 0;
		}
	}
}

void Channel::startSubscriber() {
	assert(!communications_manager);
	assert(!definition()->isPublisher()); // publishers use startServer()

	// A clockwork to clockwork connection uses its own thread to manage a subscription
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);
	//DBG_CHANNELS << "Channel " << name << " setting up subscriber thread and client side connection from thread " << tnam << "\n";

	subscriber_thread = new boost::thread(boost::ref(*this));

	// create a socket to communicate with the newly started subscriber thread
	cmd_client = createCommandSocket(true);

	// start the subcriber thread
	DBG_CHANNELS << name << " sending command start message\n";
	cmd_client->send("start",5);
	char buf[100];
	DBG_CHANNELS << name << " sent command start message\n";
	size_t buflen = cmd_client->recv(buf, 100);
	DBG_CHANNELS << name << " command start message acknowledged\n";
	buf[buflen] = 0;

	connect_responder = new ChannelConnectMonitor(this);
	disconnect_responder = new ChannelDisconnectMonitor(this);
	if (isClient())
		communications_manager->monit_subs.addResponder(ZMQ_EVENT_CONNECTED, connect_responder);
	else
		communications_manager->monit_subs.addResponder(ZMQ_EVENT_ACCEPTED, connect_responder);
	communications_manager->monit_subs.addResponder(ZMQ_EVENT_DISCONNECTED, disconnect_responder);
	DBG_CHANNELS << "Channel " << name << " got response to start: " << buf << "\n";
}

// NOTE this method is not currently called
void Channel::stopSubscriber() {
	delete communications_manager;
	communications_manager = 0;
}

void ChannelDefinition::instantiateInterfaces() {
    // find all machines listed in the updates clauses of channel definitions
    if (!all) return; // no channel definitions to look through
    std::map< std::string, ChannelDefinition* >::iterator iter = all->begin();
    while (iter != all->end()) {
        const std::pair<std::string, ChannelDefinition*> item = *iter++;
        std::map<std::string, Value>::iterator updated_machines = item.second->updates_names.begin();
        while (updated_machines != item.second->updates_names.end()) {
            const std::pair<std::string, Value> &instance_name = *updated_machines++;
            MachineInstance *found = MachineInstance::find(instance_name.first.c_str());
            if (!found) { // instantiate a shadow to represent this machine
                if (item.second) {
					DBG_CHANNELS << "Instantiating SHADOW " << instance_name.first << " for Channel " << item.second->name << "\n";
                    MachineInstance *m = MachineInstanceFactory::create(instance_name.first.c_str(),
							instance_name.second.asString().c_str(),
							MachineInstance::MACHINE_SHADOW);
                    m->setDefinitionLocation("dynamic", 0);
					m->requireAuthority(item.second->authority);
					{
						FileLogger fl(program_name);
						fl.f() << "shadow " << instance_name.first
							<< " requires authority " << item.second->authority << "\n";
					}
					MachineClass *mc = MachineClass::find(instance_name.second.asString().c_str());
                    m->setProperties(mc->properties);
                    m->setStateMachine(mc);
                    m->setValue("startup_enabled", false);
                    machines[instance_name.first] = m;
                    ::machines[instance_name.first] = m;
                }
                else {
                    char buf[150];
                    snprintf(buf, 150, "Error: no interface named %s", item.first.c_str());
                    MessageLog::instance()->add(buf);
					DBG_CHANNELS << buf << "\n";
                }
            }
        }
		std::map<std::string, Value>::iterator i_shared = item.second->shares_names.begin();
		while (i_shared != item.second->shares_names.end()) {
			const std::pair<std::string, Value> &instance_name = *i_shared++;
			MachineInstance *found = MachineInstance::find(instance_name.first.c_str());
			if (!found) { // instantiate a shadow to represent this machine
				if (item.second) {
					MachineInstance *m = MachineInstanceFactory::create(instance_name.first.c_str(),
																		instance_name.second.asString().c_str(),
																		MachineInstance::MACHINE_SHADOW);
					m->setDefinitionLocation("dynamic", 0);
					m->requireAuthority(item.second->authority);
					{
						FileLogger fl(program_name);
						fl.f() << "shadow " << instance_name.first
						<< " requires authority " << item.second->authority << "\n";
					}
					MachineClass *mc = MachineClass::find(instance_name.second.asString().c_str());
					m->setProperties(mc->properties);
					m->setStateMachine(mc);
					m->setValue("startup_enabled", false);
					machines[instance_name.first] = m;
					::machines[instance_name.first] = m;
				}
				else {
					char buf[150];
					snprintf(buf, 150, "Error: no interface named %s", item.first.c_str());
					MessageLog::instance()->add(buf);
				}
			}
		}
    }
    // channels automatically setup monitoring on machines they update
    // when the channel is instantiated. At startup, however, some machines
    // may not have been defined when the channel is created so we need
    // to do an extra pass here.
    Channel::setupAllShadows();
}

Channel *Channel::create(unsigned int port, ChannelDefinition *defn) {
    assert(defn);
    char channel_name[100];
    snprintf(channel_name, 100, "%s_%d", defn->name.c_str(), port);
    Channel *chn = new Channel(channel_name, defn->name);

    chn->setPort(port);
    chn->setDefinition(defn);
	chn->setDefinitionLocation("dynamic", 0);
    chn->setStateMachine(defn);
	if (defn->getThrottleTime())
	{
		DBG_CHANNELS << " channel " << channel_name << " is throttled (" << defn->getThrottleTime() << ")\n";
		chn->setThrottleTime(defn->getThrottleTime());
	}
    if (defn->monitors_exports) chn->monitors_exports = true;
    chn->modified();
    chn->setupShadows();
    chn->setupFilters();
    return chn;
}

void ChannelImplementation::modified() {
    struct timeval now;
    gettimeofday(&now, 0);
    last_modified = now.tv_sec * 1000000L + now.tv_usec;
}

void ChannelImplementation::checked() {
    struct timeval now;
    gettimeofday(&now, 0);
    last_checked = now.tv_sec * 1000000L + now.tv_usec;
}

static void copyJSONArrayToSet(cJSON *obj, const char *key, std::set<std::string> &res) {
    cJSON *items = cJSON_GetObjectItem(obj, key);
    if (items && items->type == cJSON_Array) {
        cJSON *item = items->child;
        while (item) {
            if (item->type == cJSON_String) res.insert(item->valuestring);
            item = item->next;
        }
    }
}

static void copyJSONArrayToMap(cJSON *obj, const char *key, std::map<std::string, Value> &res, const char *key_name = "property", const char *value_name = "type") {
    cJSON *items = cJSON_GetObjectItem(obj, key);
    if (items && items->type == cJSON_Array) {
        cJSON *item = items->child;
        while (item) {
            // we only collect items from the array that match our expected format of
            // property,value pairs
            if (item->type == cJSON_Object) {
                cJSON *js_prop = cJSON_GetObjectItem(item, key_name);
                cJSON *js_type = cJSON_GetObjectItem(item, value_name);
                cJSON *js_val = cJSON_GetObjectItem(item, "value");
                if (js_prop->type == cJSON_String) {
                    res[js_prop->valuestring] = MessageEncoding::valueFromJSONObject(js_val, js_type);
                }
            }
            item = item->next;
        }
    }
}

ChannelDefinition *ChannelDefinition::fromJSON(const char *json) {
    cJSON *obj = cJSON_Parse(json);
    if (!obj) return 0;
/*    std::map<std::string, Value> options;
*/
    cJSON *item = cJSON_GetObjectItem(obj, "identifier");
    std::string name;
    if (item && item->type == cJSON_String) {
        name = item->valuestring;
    }
    ChannelDefinition *defn = new ChannelDefinition(name.c_str());
    defn->identifier = name;
    item = cJSON_GetObjectItem(obj, "key");
    if (item && item->type == cJSON_String)
        defn->setKey(item->valuestring);
    item = cJSON_GetObjectItem(obj, "version");
    if (item && item->type == cJSON_String)
        defn->setVersion(item->valuestring);
    copyJSONArrayToSet(obj, "monitors", defn->monitors_names);
    copyJSONArrayToSet(obj, "monitors_patterns", defn->monitors_patterns);
    copyJSONArrayToMap(obj, "monitors_properties", defn->monitors_properties);
    item = cJSON_GetObjectItem(obj, "monitors_exports");
    if (item && item->type == cJSON_True)
        defn->monitors_exports = true;
    else if (item && item->type == cJSON_False)
        defn->monitors_exports = false;

    copyJSONArrayToMap(obj, "shares", defn->shares_names);
    copyJSONArrayToMap(obj, "updates", defn->updates_names);
    copyJSONArrayToSet(obj, "sends", defn->send_messages);
    copyJSONArrayToSet(obj, "receives", defn->recv_messages);
    return defn;
}

static cJSON *StringSetToJSONArray(std::set<std::string> &items) {
    cJSON *res = cJSON_CreateArray();
    std::set<std::string>::iterator iter = items.begin();
    while (iter != items.end()) {
        const std::string &str = *iter++;
        cJSON_AddItemToArray(res, cJSON_CreateString(str.c_str()));
    }
    return res;
}

static cJSON *MapToJSONArray(std::map<std::string, Value> &items, const char *key_name = "property", const char *value_name = "type") {
    cJSON *res = cJSON_CreateArray();
    std::map<std::string, Value>::iterator iter = items.begin();
    while (iter != items.end()) {
        const std::pair<std::string, Value> item = *iter++;
        cJSON *js_item = cJSON_CreateObject();
        cJSON_AddItemToObject(js_item, key_name, cJSON_CreateString(item.first.c_str()));
        cJSON_AddStringToObject(js_item, value_name, MessageEncoding::valueType(item.second).c_str());
        MessageEncoding::addValueToJSONObject(js_item, "value", item.second);
        cJSON_AddItemToArray(res, js_item);
    }
    return res;
}

char *ChannelDefinition::toJSON() {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "identifier", this->name.c_str());
    cJSON_AddStringToObject(obj, "key", this->psk.c_str());
    cJSON_AddStringToObject(obj, "version", this->version.c_str());
    cJSON_AddItemToObject(obj, "monitors", StringSetToJSONArray(this->monitors_names));
    cJSON_AddItemToObject(obj, "monitors_patterns", StringSetToJSONArray(this->monitors_patterns));
    cJSON_AddItemToObject(obj, "monitors_properties", MapToJSONArray(this->monitors_properties));
    if (this->monitors_exports)
        cJSON_AddTrueToObject(obj, "monitors_exports");
    else
        cJSON_AddFalseToObject(obj, "monitors_exports");
    cJSON_AddItemToObject(obj, "shares", MapToJSONArray(this->shares_names));
    cJSON_AddItemToObject(obj, "updates", MapToJSONArray(this->updates_names, "name","type"));
    cJSON_AddItemToObject(obj, "sends", StringSetToJSONArray(this->send_messages));
    cJSON_AddItemToObject(obj, "receives", StringSetToJSONArray(this->recv_messages));
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return str;
}



MessageHandler::MessageHandler() : data(0), data_size(0) {
}

MessageHandler::~MessageHandler() {
    if (data) free(data);
}

void MessageHandler::handleMessage(const char *buf, size_t len) {
    if (data) { free(data); data = 0; data_size = 0; }
    data = (char*)malloc(len);
    memcpy(data, buf, len);
}

bool MessageHandler::receiveMessage(zmq::socket_t &sock) {
    if (data) { free(data); data = 0; data_size = 0; }
    zmq::message_t update;
    if (sock.recv(&update, ZMQ_NOBLOCK)) {
        long len = update.size();
        data = (char *)malloc(len+1);
        memcpy(data, update.data(), len);
        data[len] = 0;
		data_size = len+1;
        return true;
    }
    return false;
}


Channel *Channel::find(const std::string name) {
    if (!all) return 0;
    std::map<std::string, Channel *>::iterator found = all->find(name);
    if (found == all->end()) return 0;
    return (*found).second;
}

void Channel::remove(const std::string name) {
    if (!all) return;
    std::map<std::string, Channel *>::iterator found = all->find(name);
    if (found == all->end()) return;
    all->erase(found);
}

void Channel::setDefinition(const ChannelDefinition *def) {
    definition_ = def;
}

void Channel::sendPropertyChangeMessage(const std::string &name, const Value &key, const Value &val) {
	if (communications_manager) {
		std::string response;
		char *cmd = MessageEncoding::encodeCommand("PROPERTY", name, key, val); // send command
		if (isClient())
				//sendMessage(cmd, communications_manager->subscriber(),response); //setup()
				safeSend(communications_manager->subscriber(), cmd, strlen(cmd) );
			else
				//sendMessage(cmd, communications_manager->subscriber(),response);
				safeSend(communications_manager->subscriber(), cmd, strlen(cmd));
			//DBG_CHANNELS << "channel " << name << " got response: " << response << "\n";
			free(cmd);
	}
	else if (mif) {
		char *cmd = MessageEncoding::encodeCommand("PROPERTY", name, key, val); // send command
		mif->send(cmd);
		free(cmd);
	}
}


void Channel::sendPropertyChange(MachineInstance *machine, const Value &key, const Value &val) {
    if (!all) return;
    std::string name = machine->fullName();
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        Channel *chn = (*iter).second; iter++;
			if (!chn->definition()->hasFeature(ChannelDefinition::ReportPropertyChanges)) continue;
			if (chn->current_state != ChannelImplementation::ACTIVE) continue;
		//if (machine->ownerChannel() == chn) continue; // shadows don't forward their properties back on their channel
        if (!chn->channel_machines.count(machine))
            continue;
        if (chn->filtersAllow(machine)) {
			if (chn->throttle_time && machine->needsThrottle()) {
				DBG_CHANNELS << chn->getName() << " throttling " << machine->getName() << " " << key << "\n";
				if (!chn->throttled_items[machine])
					chn->throttled_items[machine] = new MachineRecord(machine);
				chn->throttled_items[machine]->properties[key.asString()] = val;
			}
			else {
				chn->sendPropertyChangeMessage(machine->getName(), key, val);
			}
        }
    }
}

// step through the list of properties that have been throttled on this channel
// and send the property change and/or the modbus update depending on channel features
void Channel::sendThrottledUpdates() {
	if (current_state != ChannelImplementation::ACTIVE)
		return;
	if ( !communications_manager || communications_manager->monit_subs.disconnected()) {
		return;
	}
	bool do_modbus = definition()->hasFeature(ChannelDefinition::ReportModbusUpdates);
	bool do_properties = definition()->hasFeature(ChannelDefinition::ReportPropertyChanges);
	last_throttled_send = nowMicrosecs();
	std::map<MachineInstance *, MachineRecord*>::iterator iter = throttled_items.begin();
	while (iter != throttled_items.end()) {
		std::pair<MachineInstance *, MachineRecord*> item = *iter;
		if (do_properties || do_modbus){
			if (do_properties) {
				std::map<std::string, Value> *props = &item.second->properties;
				std::map<std::string, Value>::iterator props_iter = props->begin();
				while (props_iter != props->end()) {
					std::pair<std::string, Value> prop = *props_iter;
					Value key(prop.first);
					sendPropertyChangeMessage(item.second->machine->getName(), key, prop.second);
					if (!do_modbus)
						props_iter = props->erase(props_iter);
					else props_iter++;
				}
			}
			if (do_modbus) {
				std::map<std::string, Value> *props = &item.second->properties;
				std::map<std::string, Value>::iterator props_iter = props->begin();
				while (props_iter != props->end()) {
					std::pair<std::string, Value> prop = *props_iter;
					MachineRecord *m = item.second;
					if (m && m->machine)
						m->machine->sendModbusUpdate(prop.first, prop.second);
					else
						assert(false);
					props_iter = props->erase(props_iter);
				}
			}
		}
		else
			assert(false); // should not be here if we aren't doing properties or modbus messages
		delete item.second;
		iter = throttled_items.erase(iter);
	}
}

// send the property changes that have been recorded for this machine
void Channel::sendPropertyChanges(MachineInstance *machine) {
    if (!all) return;
    std::string name = machine->fullName();
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        Channel *chn = (*iter).second; iter++;
			bool do_modbus = chn->definition()->hasFeature(ChannelDefinition::ReportModbusUpdates);
			bool do_properties = chn->definition()->hasFeature(ChannelDefinition::ReportPropertyChanges);
			if (chn->current_state == ChannelImplementation::DISCONNECTED) continue;
			if (!do_modbus && !do_properties) continue;

        if (!chn->channel_machines.count(machine))
            continue;
				
		if (!chn->throttle_time) continue;

		std::map<MachineInstance *, MachineRecord*>::iterator found = chn->throttled_items.find(machine);
		if (chn->filtersAllow(machine) && found != chn->throttled_items.end() ) {
			MachineRecord *mr = (*found).second;
			std::map<std::string, Value>::iterator iter = mr->properties.begin();
			while (iter != mr->properties.end()) {
				std::pair<std::string, Value> item = *iter;
				Value key(item.first);
				if (do_properties)
					chn->sendPropertyChangeMessage(machine->getName(), key, item.second);
				if (do_modbus)
					machine->sendModbusUpdate(item.first, item.second);
				iter = mr->properties.erase(iter);
			}
			found = chn->throttled_items.erase(found);
			delete mr;
		}
    }
}

bool Channel::matches(MachineInstance *machine, const std::string &name) {
    if (definition()->monitors_names.count(name))
        return true;
    else if (channel_machines.count(machine))
        return true;
    return false; // only test the channel instance if the channel definition matches
}

bool Channel::patternMatches(const std::string &machine_name) {
    // no match on name but we still may match on pattern
    std::set<std::string>::iterator iter = monitors_patterns.begin();
    while (iter != monitors_patterns.end()) {
        const std::string &pattern = *iter++;
        rexp_info *rexp = create_pattern(pattern.c_str());
        if (!rexp->compilation_error) {
            if (execute_pattern(rexp, machine_name.c_str()) == 0) {
                return true;
            }
        }
        else {
            MessageLog::instance()->add(rexp->compilation_error);
            DBG_CHANNELS << "Channel error: " << name << " " << rexp->compilation_error << "\n";
            return false;
        }
    }
    return false;
}

bool Channel::doesUpdate() {
    return definition()->updates_names.empty();
}

bool Channel::doesShare() {
	return definition()->shares_names.empty();
}

bool Channel::doesMonitor() {
    return  monitors_exports || definition()->monitors_exports
        || ! (definition()->monitors_names.empty() && definition()->monitors_patterns.empty() && definition()->monitors_properties.empty()
              && monitors_names.empty() && monitors_patterns.empty() && monitors_properties.empty());
}

bool Channel::filtersAllow(MachineInstance *machine) {
		if (!definition()->monitor_linked.empty() && ( machine->_type == "INPUTBIT" || machine->_type == "INPUTREGISTER") ) return false;
    if ((definition()->monitors_exports || monitors_exports) && !machine->modbus_exports.empty())
        return true;
    
    if (!matches(machine, name)) return false;
    
    if (monitors_names.empty() && monitors_patterns.empty() && monitors_properties.empty())
        return true;
    
    // apply channel specific filters
    size_t n = monitors_names.count(name);
    if (!monitors_names.empty() && n)
        return true;
    
    // if patterns are given and this machine doesn't match anything else, we try patterns
    if (!monitors_patterns.empty()) {
        if (patternMatches(machine->getName()))
            return true;
    }
    
    // if properties are given and this machine doesn't match anything else, we try properties
    if (!monitors_properties.empty()) {
        // TBD check properties
    }
    return false;
}

Channel *Channel::findByType(const std::string kind) {
    if (!all) return 0;
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        Channel *chn = (*iter).second; iter++;
        if (chn->_type == kind) return chn;
    }
    return 0;
}

bool Channel::throttledItemsReady(uint64_t now_usecs) const {
	return !throttled_items.empty() && now_usecs - last_throttled_send >= throttle_time;
}

void Channel::sendModbusUpdate(MachineInstance *machine, const std::string &property_name, const Value &new_value) {
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);

	if (!all) return;
	std::map<std::string, Channel*>::iterator iter = all->begin();
	while (iter != all->end()) {
		Channel *chn = (*iter).second; iter++;
		if (chn->current_state == ChannelImplementation::DISCONNECTED) continue;
		if (!chn->definition()->hasFeature(ChannelDefinition::ReportModbusUpdates)) continue;

		//TBD change modbus channels to send data using a guaranteed delivery method
		//currently modbus channels publish data to all connected channels.
		//

		if (chn->throttle_time && machine->needsThrottle()) {
			DBG_CHANNELS << chn->getName() << " throttling " << machine->getName() << " " << property_name << "\n";
			if (!chn->throttled_items[machine])
				chn->throttled_items[machine] = new MachineRecord(machine);
			chn->throttled_items[machine]->properties[property_name] = new_value;
		}
		else {
			machine->sendModbusUpdate(property_name, new_value);
		}
		//return; // since modbus channels are publishers we only need to send once
	}
}

void Channel::sendStateChange(MachineInstance *machine, std::string new_state, uint64_t auth) {
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);

	MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(machine);

	if (!all) return;
    std::string machine_name = machine->fullName();
	char *cmdstr = 0;


	std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        Channel *chn = (*iter).second; iter++;
		if (chn->current_state == ChannelImplementation::DISCONNECTED) continue;
		if (!chn->definition()->hasFeature(ChannelDefinition::ReportStateChanges)) continue;

		if (!chn->channel_machines.count(machine))
			continue;

		// shadow machines use the authority provided by the caller to effect the
		// state change but 'real' devices escalate to the channel's authority to
		// make sure that shadow listen.

        if (chn->filtersAllow(machine)) {

			if (!chn->definition()->isPublisher()) {
				if (machine->isShadow()) {
					cmdstr = MessageEncoding::encodeState(machine_name, new_state, auth);
				}
				else {
					NB_MSG << "using authority " << chn->getAuthority()
					<< " to set " << machine_name << " to " << new_state << "\n";
					cmdstr = MessageEncoding::encodeState(machine_name, new_state, chn->getAuthority());
				}
			}
			else // publisher channels do not use the authority parameter on state changes
				cmdstr = MessageEncoding::encodeState(machine_name, new_state);

			if (!chn->isClient() && chn->communications_manager) {
				std::string response;
				safeSend(chn->communications_manager->subscriber(), cmdstr, strlen(cmdstr) );
			}
            else if (chn->communications_manager
                  && chn->communications_manager->setupStatus() == SubscriptionManager::e_done ) {

				if (!chn->definition()->isPublisher()) {
					std::string response;
					//chn->sendMessage(cmdstr, chn->communications_manager->subscriber(),response);
					safeSend(chn->communications_manager->subscriber(), cmdstr, strlen(cmdstr) );
					//DBG_CHANNELS << tnam << ": channel " << chn->name << " got response: " << response << "\n";
				}
				else {
					safeSend(chn->communications_manager->subscriber(), cmdstr, strlen(cmdstr) );
				}
            }
            else if (chn->mif) {
                chn->mif->send(cmdstr);
            }
            else {
                char buf[150];
                snprintf(buf, 150,
					 "Warning: machine %s changed state but the channel is not connected",
					 machine->getName().c_str());
                MessageLog::instance()->add(buf);
            }
			free(cmdstr);
        }
    }
}

void Channel::requestStateChange(MachineInstance *machine, std::string new_state, uint64_t auth) {
	// this is an instance based version of the above
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);

	MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(machine);

	std::string machine_name = machine->fullName();
	char *cmdstr = 0;

	Channel *chn = this;
	if (current_state == ChannelImplementation::DISCONNECTED) return;
	if (!definition()->hasFeature(ChannelDefinition::ReportStateChanges)) return;
	if (!channel_machines.count(machine)) return;

	// shadow machines use the authority provided by the caller to effect the
	// state change but 'real' devices escalate to the channel's authority to
	// make sure that shadow listen.

	if (filtersAllow(machine)) {

		if (!definition()->isPublisher()) {
			if (machine->isShadow()) {
				cmdstr = MessageEncoding::encodeState(machine_name, new_state, auth);
			}
			else {
				NB_MSG << "using authority " << chn->getAuthority()
				<< " to set " << machine_name << " to " << new_state << "\n";
				cmdstr = MessageEncoding::encodeState(machine_name, new_state, chn->getAuthority());
			}
		}
		else // publisher channels do not use the authority parameter on state changes
			cmdstr = MessageEncoding::encodeState(machine_name, new_state);

		if (!isClient() && communications_manager) {
			std::string response;
			safeSend(communications_manager->subscriber(), cmdstr, strlen(cmdstr) );
		}
		else if (communications_manager
				 && communications_manager->setupStatus() == SubscriptionManager::e_done ) {

			if (!definition()->isPublisher()) {
				std::string response;
				//chn->sendMessage(cmdstr, chn->communications_manager->subscriber(),response);
				safeSend(communications_manager->subscriber(), cmdstr, strlen(cmdstr) );
				//DBG_CHANNELS << tnam << ": channel " << chn->name << " got response: " << response << "\n";
			}
			else {
				safeSend(communications_manager->subscriber(), cmdstr, strlen(cmdstr) );
			}
		}
		else if (mif) {
			mif->send(cmdstr);
		}
		else {
			char buf[150];
			snprintf(buf, 150,
					 "Warning: machine %s changed state but the channel is not connected",
					 machine->getName().c_str());
			MessageLog::instance()->add(buf);
		}
		free(cmdstr);
	}
}

void Channel::sendCommand(MachineInstance *machine, std::string command, std::list<Value>*params) {
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);
	DBG_CHANNELS << " sending " << command << " to channels that monitor " << machine->getName() << "\n";

	if (!all) return;
	std::string name = machine->fullName();
	std::map<std::string, Channel*>::iterator iter = all->begin();
	while (iter != all->end()) {
		Channel *chn = (*iter).second; iter++;
		if (chn->current_state == ChannelImplementation::DISCONNECTED) continue;
		if (command == "UPDATE" && !chn->definition()->hasFeature(ChannelDefinition::ReportModbusUpdates))
			continue;

		if (!chn->channel_machines.count(machine))
			continue;
		if (chn->filtersAllow(machine)) {
			if ( (!chn->isClient() && chn->communications_manager)
					 || ( chn->isClient() && chn->communications_manager
						&& chn->communications_manager->setupStatus() == SubscriptionManager::e_done) ) {
				std::string response;
				char *cmd = MessageEncoding::encodeCommand(command, params); // send command
				DBG_CHANNELS << "Channel " << chn->name << " sending " << cmd << "\n";
				chn->sendMessage(cmd, chn->communications_manager->subscriber(),response);//setup()
				free(cmd);
			}
			else if (chn->mif) {
				char *cmd = MessageEncoding::encodeCommand(command, params); // send command
				DBG_CHANNELS << "Channel " << name << " sending " << cmd << "\n";
				chn->mif->send(cmd);
				free(cmd);
			}
			else {
				char buf[150];
				snprintf(buf, 150, "Warning: machine %s should send %s but the channel is not connected",
						machine->getName().c_str(), command.c_str() );
				MessageLog::instance()->add(buf);
				NB_MSG << buf << "\n";
			}
		}
		else {
			DBG_CHANNELS << "message on channel " << chn->name << " skipped as it does not match filters\n";
		}
	}
}


void ChannelDefinition::setKey(const char *s) {
    psk = s;
}

void ChannelDefinition::setIdent(const char *s) {
    identifier = s;
}
void ChannelDefinition::setVersion(const char *s) {
    version = s;
}
void ChannelDefinition::addShare(const char *nm, const char *if_nm) {
	shares_names[nm] = if_nm;
	modified();
}
void ChannelImplementation::addMonitor(const char *s) {
    DBG_CHANNELS << "add monitor for " << s << "\n";
    monitors_names.insert(s);
    modified();
}
void ChannelImplementation::addIgnorePattern(const char *s) {
    DBG_CHANNELS << "add " << s << " to ignore list\n";
	ignores_patterns.insert(s);
    modified();
}
void ChannelImplementation::removeIgnorePattern(const char *s) {
    DBG_CHANNELS << "remove " << s << " from ignore list\n";
    ignores_patterns.erase(s);
    modified();
}
void ChannelImplementation::removeMonitor(const char *s) {
    DBG_CHANNELS << "remove monitor for " << s;
	if (monitors_names.count(s)) {
	    monitors_names.erase(s);
    	modified();
		DBG_CHANNELS << "\n";
	}
	else {
		DBG_CHANNELS << "...not found\n";
		std::string pattern = "^";
		pattern += s;
		pattern += "$";
		addIgnorePattern(pattern.c_str());
	}
}
void ChannelImplementation::addMonitorPattern(const char *s) {
    monitors_patterns.insert(s);
    modified();
}
void ChannelImplementation::addMonitorProperty(const char *key, const Value &val) {
    monitors_properties[key] = val;
    modified();
}
void ChannelImplementation::removeMonitorProperty(const char *key, const Value &val) {
    monitors_properties.erase(key);
    modified();
    //TBD Bug here, we should be using a set< pair<string, Value> >, not a map
}
void ChannelImplementation::addMonitorExports() {
    monitors_exports = true;
    modified();
}
void ChannelImplementation::removeMonitorExports() {
    monitors_exports = false;
    modified();
}

void ChannelImplementation::addMonitorLinkedTo(const char *machine_name) {
	monitor_linked.insert(machine_name);
}

void ChannelImplementation::removeMonitorPattern(const char *s) {
    monitors_patterns.erase(s);
    modified();
}
void ChannelDefinition::addUpdates(const char *nm, const char *if_nm) {
    updates_names[nm] = if_nm;
    modified();
}
void ChannelDefinition::addShares(const char *nm, const char *if_nm) {
	shares_names[nm] = if_nm;
	modified();
}
void ChannelDefinition::addSendName(const char *s) {
    send_messages.insert(s);
    modified();
}
void ChannelDefinition::addReceiveName(const char *s) {
    recv_messages.insert(s);
    modified();
}
void ChannelDefinition::addOptionName(const char *n, Value &v) {
    options[n] = v;
    modified();
}

/* When a channel is created and connected, instances of updated machines on that channel are enabled.
 */
#include "EnableAction.h"

void Channel::enableShadows() {
    // enable all machines that are updated by this channel
	// don't perform the enable operation here though,
	// queue them for later

	// shadow devices on the client side need to have their authorisatio key set
	// to the same as devices on the server side.
	if (isClient()) {
		authority = communications_manager->authority;
	}
    std::map<std::string, Value>::const_iterator iter = definition()->updates_names.begin();
    while (iter != definition()->updates_names.end()) {
        const std::pair< std::string, Value> item = *iter++;
        MachineInstance *m = MachineInstance::find(item.first.c_str());
        MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(m);
		if (ms) {
			if (isClient()) ms->requireAuthority(authority);
			//DBG_CHANNELS << "Channel " << name << " enabling shadow machine " << ms->getName() << "\n";
			channel_machines.insert(ms); // ensure the channel is linked to the shadow machine
			EnableActionTemplate ea(ms->getName().c_str());
			enqueueAction(ea.factory(ms));
		}
    }
	iter = definition()->shares_names.begin();
	while (iter != definition()->shares_names.end()) {
		const std::pair< std::string, Value> item = *iter++;
		MachineInstance *m = MachineInstance::find(item.first.c_str());
		MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(m);
		if (ms) {
			//DBG_CHANNELS << "Channel " << name << " enabling shadow machine " << ms->getName() << "\n";
			channel_machines.insert(ms); // ensure the channel is linked to the shadow machine
			EnableActionTemplate ea(ms->getName().c_str());
			enqueueAction(ea.factory(ms));
		}
	}
}

void Channel::disableShadows() {
    // disable all machines that are updated by this channel
    std::map<std::string, Value>::const_iterator iter = definition()->updates_names.begin();
    while (iter != definition()->updates_names.end()) {
        const std::pair< std::string, Value> item = *iter++;
        MachineInstance *m = MachineInstance::find(item.first.c_str());
        MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(m);
		if (ms) {
			//DBG_CHANNELS << "Channel " << name << " disabling shadow machine " << ms->getName() << "\n";
			ms->disable();
		}
    }
	iter = definition()->shares_names.begin();
	while (iter != definition()->shares_names.end()) {
		const std::pair< std::string, Value> item = *iter++;
		MachineInstance *m = MachineInstance::find(item.first.c_str());
		MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(m);
		if (ms) {
			//DBG_CHANNELS << "Channel " << name << " disabling shadow machine " << ms->getName() << "\n";
			ms->disable();
		}
	}
}

void Channel::setupShadows() {
    // decide if the machines updated on this channel are instantiated as
    // normal machines and if so, publish their changes to this channel.
    if (!definition_) definition_ = ChannelDefinition::find(this->_type.c_str());
    if (!definition_) {
        char buf[150];
        snprintf(buf, 150, "Error: channel definition %s for %s could not be found", name.c_str(), _type.c_str());
        MessageLog::instance()->add(buf);
        return;
    }
    
    std::map<std::string, Value>::const_iterator iter = definition()->updates_names.begin();
    while (iter != definition()->updates_names.end()) {
        const std::pair< std::string, Value> item = *iter++;
        MachineInstance *m = MachineInstance::find(item.first.c_str());
        MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(m);
        if (m && !ms) { // this machine is not a shadow.
            if (!channel_machines.count(m)) {
                m->publish();
                channel_machines.insert(m);
            }
        }
        else if (m) {
			m->publish();
            // this machine is a shadow
			channel_machines.insert(m);
			m->owner_channel = this;
        }
    }

	iter = definition()->shares_names.begin();
	while (iter != definition()->shares_names.end()) {
		const std::pair< std::string, Value> item = *iter++;
		MachineInstance *m = MachineInstance::find(item.first.c_str());
		MachineShadowInstance *ms = dynamic_cast<MachineShadowInstance*>(m);
		if (m && !ms) { // this machine is not a shadow.
			if (!channel_machines.count(m)) {
				m->publish();
				channel_machines.insert(m);
			}
		}
		else if (m) {
			// this machine is a shadow
		}
	}


    //- TBD should this only subscribe if channel monitors a machine?
}

#if 0
// poll channels and return number of descriptors with activity
// if n is 0 the block of data will be reallocated,
// otherwise up to n items will be initialised
int Channel::pollChannels(zmq::pollitem_t * &poll_items, long timeout, int n) {
    if (!all) return 0;
    if (!n) {
        std::map<std::string, Channel*>::iterator iter = all->begin();
        while (iter != all->end()) {
            const std::pair<std::string, Channel *> &item = *iter++;
            Channel *chn = item.second;
            if (chn->communications_manager) n += chn->communications_manager->numSocks();
        }
        if (poll_items) delete poll_items;
		poll_items = 0;
        if (n) poll_items = new zmq::pollitem_t[n];
    }
	if (!poll_items) return 0;

    int count = 0;
    zmq::pollitem_t *curr = poll_items;
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        const std::pair<std::string, Channel *> &item = *iter++;
        Channel *chn = item.second;
        if (chn->communications_manager) {
            chn->communications_manager->checkConnections();
            if (chn->communications_manager && n >= count + chn->communications_manager->numSocks()) {
                count += chn->communications_manager->configurePoll(curr);
                chn->setPollItemBase(curr);
                curr += count;
            }
        }
    }
    int rc = 0;
    while (true) {
        try {
            
            int rc = zmq::poll(poll_items, count, timeout);
            if (rc == -1 && errno == EAGAIN) continue;
            break;
        }
        catch(zmq::error_t e) {
            if (errno == EINTR) continue;
            char buf[150];
            snprintf(buf, 150, "Channel error: %s", zmq_strerror(errno));
            MessageLog::instance()->add(buf);
            DBG_CHANNELS << buf << "\n";
            break;
        }
    }
    //if (rc>0) DBG_CHANNELS << rc << " channels with activity\n";
    return rc;
}
#endif

void Channel::newPendingCommand(IODCommand *cmd) {
	chn_scoped_lock lock("newPending", internals->iod_cmd_get_mutex);

	internals->pending_commands.push_back(cmd);
}

IODCommand *Channel::getCommand() {
	chn_scoped_lock lock("getCommand", internals->iod_cmd_get_mutex);

	if (internals->pending_commands.empty()) return 0;

	IODCommand *cmd = internals->pending_commands.front();
	/*
	for (int i=0; i<10; ++i ) {
		assert(cmd == internals->pending_messages.front());
		usleep(5);
	}
	*/
	internals->pending_commands.pop_front();
	return cmd;
}

IODCommand *Channel::getCompletedCommand() {
	chn_scoped_lock ("getCompleted", internals->iod_cmd_put_mutex);

	if (internals->completed_commands.empty()) return 0;

	IODCommand *cmd = internals->completed_commands.front();
	internals->completed_commands.pop_front();
	return cmd;
}

void Channel::putCompletedCommand(IODCommand *cmd) {
	chn_scoped_lock ("putCompleted", internals->iod_cmd_put_mutex);

	internals->completed_commands.push_back(cmd);
}


// This method is executed on the main thread
void Channel::handleChannels() {
	if (!all) return;
	uint64_t now = nowMicrosecs();
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        const std::pair<std::string, Channel *> &item = *iter++;
        Channel *chn = item.second;
        chn->checkCommunications();
		if (chn->throttledItemsReady(now)) {
			chn->sendThrottledUpdates();
		}
		IODCommand *command = chn->getCommand();
		while (command) {
			{FileLogger fl(program_name);
			fl.f() << "Processing: received command: " << *command << " on channel " << chn->name << "\n";}
			//hack?
			if (command->param(0) == "STATE"
				&& command->numParams() == 4) {
				if ( chn->isClient() && command->param(3) == (long)chn->getAuthority()) {
					// do nothing, this is an echo of a command we sent
					{FileLogger fl(program_name); fl.f() << "skipping echoed command\n"; }
				}
				else if ( !chn->isClient() && command->param(3) == (long)chn->definition()->getAuthority()) {
					// do nothing, this is an echo of a command we sent
					{FileLogger fl(program_name); fl.f() << "skipping echoed command\n"; }
				}
				else {
					{FileLogger fl(program_name);
						fl.f() << chn->name << " processing echoed command "
						<< chn->getAuthority() << " != " << command->param(3) << "\n"; }
					(*command)();
				}
			}
			else
				(*command)();
			{FileLogger fl(program_name);
			fl.f() << chn->name << "posting completed command: " << *command << " on channel " << chn->name << "\n";}
			chn->putCompletedCommand(command);
			command = chn->getCommand();
		}
	}
}

#if 0
void Channel::setPollItemBase(zmq::pollitem_t *base) {
    poll_items = base;
}
#endif

void Channel::enable() {
	if (enabled()) {
		DBG_CHANNELS << "Channel " << name << " is already enabled\n";
	}
	else {
		MachineInstance::enable();
	}
}

void Channel::disable() {
}

// This method is executed on the main thread
void Channel::checkCommunications() {
#if 0
	char tnam[100];
	int pgn_rc = pthread_getname_np(pthread_self(),tnam, 100);
	assert(pgn_rc == 0);
	DBG_CHANNELS << "Channel " << name << " checkCommunications on thread "<< tnam << "\n";
#endif
	if (!communications_manager) return;
	if (!communications_manager->ready()) return;
    bool ok = communications_manager->checkConnections();
    if (!ok) {
		if (isClient()) {
			if (communications_manager->monit_setup->disconnected()
				&& current_state != ChannelImplementation::DISCONNECTED)
				setState(ChannelImplementation::DISCONNECTED);
			else if (!communications_manager->monit_setup->disconnected()) {
				if (communications_manager->setupStatus() == SubscriptionManager::e_waiting_connect
					&& current_state != ChannelImplementation::CONNECTING) {
						setState(ChannelImplementation::CONNECTING);
				}
			}
		}
        return;
	}
	if ( current_state == ChannelImplementation::DISCONNECTED) {
		setState(ChannelImplementation::CONNECTED);
	}
	else if ( current_state == ChannelImplementation::CONNECTED
			|| current_state == ChannelImplementation::CONNECTING) {
		if (isClient())
			setState(ChannelImplementation::DOWNLOADING);
		else
			setState(ChannelImplementation::UPLOADING);
	}

#if 0
    if ( poll_items && !(poll_items[1].revents & ZMQ_POLLIN) && message_handler) {
        if (message_handler->receiveMessage(communications_manager->subscriber())) {
            DBG_CHANNELS << "Channel got message: " << message_handler->data << "\n";
		}
    }
#endif
	//if (monit_subs && !monit_subs->disconnected()){
	
#if 0
	boost::unique_lock<boost::mutex> lock1(internals->iod_cmd_put_mutex, boost::defer_lock);
	boost::unique_lock<boost::mutex> lock2(internals->iod_cmd_get_mutex, boost::defer_lock);
	//NB_MSG << "Trying pending queue\n";
	if (lock2.try_lock()) {
		//NB_MSG << "pending queue locked\n";
		//NB_MSG << "Trying completed queue\n";
		if (lock1.try_lock()) {
			std::list<IODCommand*>::iterator iter = internals->pending_commands.begin();
			//NB_MSG << "completed queue locked\n";
			while (iter != internals->pending_commands.end()) {
				IODCommand *command = *iter;
				//NB_MSG << name << " processing loop got command " << command->param(0) << "\n";
				assert(command);
				try {
					(*command)();
				}
				catch (zmq::error_t zex){
					//NB_MSG << "Exception when executing command\n";
					{FileLogger fl(program_name);
						fl.f() << "Exception when executing command\n";
					}
				}
				catch (std::exception zex){
					//NB_MSG << "Exception when executing command\n";
					{FileLogger fl(program_name);
					fl.f() << "Exception when executing command\n";
					}
				}
				NB_MSG << "Channel " << name << " processed command " << command->param(0) << "\n";
				iter = internals->pending_commands.erase(iter);
				internals->completed_commands.push_back(command);
			}
			//NB_MSG << "unlocking completed queue\n";
			lock1.unlock();
		}
		//NB_MSG << "unlocking pending queue\n";
		lock2.unlock();
	}
#endif
}

void Channel::setupAllShadows() {
    if (!all) return;
    std::map<std::string, Channel*>::iterator iter = all->begin();
    while (iter != all->end()) {
        Channel *chn = (*iter).second; iter++;
        if (chn) chn->setupShadows();
    }
}

void Channel::setupFilters() {
    checked();
    // check if this channel monitors exports and if so, add machines that have exports
    if (definition()->monitors_exports || monitors_exports) {
        std::list<MachineInstance*>::iterator m_iter = MachineInstance::begin();
        while (m_iter != MachineInstance::end()) {
            MachineInstance *machine = *m_iter++;
            if (! machine->modbus_exports.empty() ) {
                this->channel_machines.insert(machine);
                machine->publish();
            }
        }
    }

	if (definition()->monitor_linked.size()) {
		std::set<std::string>::const_iterator iter = definition()->monitor_linked.begin();
		while (iter != definition()->monitor_linked.end()) {
			const std::string &machine_name = *iter++;
			MachineInstance *master = MachineInstance::find(machine_name.c_str());
			if (!master) {
				char buf[150];
				snprintf(buf, 150, "Channel %s cannot find master machine %s", name.c_str(), machine_name.c_str());
				MessageLog::instance()->add(buf);
				DBG_CHANNELS << buf << "\n";
			}
			std::list<MachineInstance*>::iterator m_iter = MachineInstance::begin();
			while (m_iter != MachineInstance::end()) {
				MachineInstance *machine = *m_iter++;
				if (machine && machine->listens.count(master) && !this->channel_machines.count(machine)) {
					machine->publish();
					this->channel_machines.insert(machine);
				}
			}
		}
	}

    std::set<std::string>::iterator iter = definition()->monitors_patterns.begin();
    while (iter != definition()->monitors_patterns.end()) {
        const std::string &pattern = *iter++;
        rexp_info *rexp = create_pattern(pattern.c_str());
        if (!rexp->compilation_error) {
            std::list<MachineInstance*>::iterator m_iter = MachineInstance::begin();
            while (m_iter != MachineInstance::end()) {
                MachineInstance *machine = *m_iter++;
                if (machine && execute_pattern(rexp, machine->getName().c_str()) == 0) {
                    if (!this->channel_machines.count(machine)) {
                        machine->publish();
                        this->channel_machines.insert(machine);
                    }
                }
            }
        }
        else {
            MessageLog::instance()->add(rexp->compilation_error);
            DBG_CHANNELS << "Channel error: " << definition()->name << " " << rexp->compilation_error << "\n";
        }
    }
    iter = definition()->monitors_names.begin();
    while (iter != definition()->monitors_names.end()) {
        const std::string &name = *iter++;
        MachineInstance *machine = MachineInstance::find(name.c_str());
        if (machine && !this->channel_machines.count(machine)) {
            machine->publish();
            this->channel_machines.insert(machine);
        }
    }
    std::map<std::string, Value>::const_iterator prop_iter = definition()->monitors_properties.begin();
    while (prop_iter != definition()->monitors_properties.end()) {
        const std::pair<std::string, Value> &item = *prop_iter++;
        std::list<MachineInstance*>::iterator m_iter = MachineInstance::begin();
        //DBG_CHANNELS << "setting up channel: searching for machines where " <<item.first << " == " << item.second << "\n";
        while (m_iter != MachineInstance::end()) {
            MachineInstance *machine = *m_iter++;
            if (machine && !this->channel_machines.count(machine)) {
                const Value &val = machine->getValue(item.first);
                // match if the machine has the property and Null was given as the match value
                //  or if the machine has the property and it matches the provided value
                if ( val != SymbolTable::Null &&
                        (item.second == SymbolTable::Null || val == item.second) ) {
                    //DBG_CHANNELS << "found match " << machine->getName() <<"\n";
                    this->channel_machines.insert(machine);
                    machine->publish();
                }
            }
        }
    }
    
    iter = definition()->ignores_patterns.begin();
    while (iter != definition()->ignores_patterns.end()) {
	    const std::string &pattern = *iter++;
	    rexp_info *rexp = create_pattern(pattern.c_str());
	    if (!rexp->compilation_error) {
		    std::list<MachineInstance*>::iterator machines = MachineInstance::begin();
		    while (machines != MachineInstance::end()) {
			    MachineInstance *machine = *machines++;
			    if (machine && execute_pattern(rexp, machine->getName().c_str()) == 0) {
				    if (this->channel_machines.count(machine)) {
					    //DBG_CHANNELS << "unpublished " << machine->getName() << "\n";
					    machine->unpublish();
					    this->channel_machines.erase(machine);
				    }
			    }
		    }
	    }
	    else {
		    MessageLog::instance()->add(rexp->compilation_error);
		    DBG_CHANNELS << "Channel error: " << definition()->name << " " << rexp->compilation_error << "\n";
	    }
    }
}

