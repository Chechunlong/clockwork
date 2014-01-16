/*
  Copyright (C) 2012 Martin Leadbeater, Michael O'Connor

  This file is part of Latproc

  Latproc is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
  
  Latproc is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Latproc; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <cassert>
#include <iostream>
#include <algorithm>
#include <sys/time.h>
#include <time.h>
#include <utility>
#include <vector>
#include "MachineInstance.h"
#include "State.h"
#include "Dispatcher.h"
#include "Message.h"
#include "IOComponent.h"
#include "Logger.h"
#include "MessagingInterface.h"
#include "DebugExtra.h"
#include "Scheduler.h"
#include "IfCommandAction.h"
#include "Expression.h"
#include "FireTriggerAction.h"
#include "HandleMessageAction.h"
#include "ExecuteMessageAction.h"
#include "CallMethodAction.h"
#include "dynamic_value.h"
#include "options.h"
#include "MessageLog.h"

extern int num_errors;
extern std::list<std::string>error_messages;

MessagingInterface *persistentStore = 0;

Parameter::Parameter(Value v) : val(v), machine(0) {
    ;
}
Parameter::Parameter(const char *name, const SymbolTable &st) : val(name), properties(st), machine(0) { }
std::ostream &Parameter::operator<< (std::ostream &out)const {
    return out << val << "(" << properties << ")";
}
Parameter::Parameter(const Parameter &orig) {
    val = orig.val; machine = orig.machine; properties = orig.properties;
}


bool Action::debug() {
	return owner && owner->debug();
}

void Action::release() { 
	--refs;
    if (refs < 0) {
		NB_MSG << "detected potential double delete of " << *this << "\n";
	}
    if (refs == 0)
        delete this;
}

Transition::Transition(State s, State d, Message t, Predicate *p) : source(s), dest(d), trigger(t), condition(0) {
    if (p) {
        condition = new Condition(p);
    }
}

Transition::Transition(const Transition &other) : source(other.source), dest(other.dest), trigger(other.trigger), condition(0) {
    if (other.condition && other.condition->predicate) {
        condition = new Condition(new Predicate(*other.condition->predicate));
    }
}

Transition::~Transition() {
    delete condition;
}

Transition &Transition::operator=(const Transition &other) {
    source = other.source;
    dest = other.dest;
    trigger = other.trigger;
    if (other.condition) {
        delete condition;
        condition = new Condition(other.condition->predicate);
    }
    return *this;
}



ConditionHandler::ConditionHandler(const ConditionHandler &other)
    : condition(other.condition),
    command_name(other.command_name),
    flag_name(other.flag_name),
    timer_val(other.timer_val),
    action(other.action),
    trigger(other.trigger),
    uses_timer(other.uses_timer),
    triggered(other.triggered)
{
    if (trigger) trigger->retain();
}

ConditionHandler &ConditionHandler::operator=(const ConditionHandler &other) {
    condition = other.condition;
    command_name = other.command_name;
    flag_name = other.flag_name;
    timer_val = other.timer_val;
    action = other.action;
    trigger = other.trigger;
    if (trigger) trigger->retain();
	uses_timer = other.uses_timer;
    triggered = other.triggered;
    return *this;
}

bool ConditionHandler::check(MachineInstance *machine) {
    if (!machine) return false;
    if (command_name == "FLAG" ) {
        MachineInstance *flag = machine->lookup(flag_name);
        if (!flag)
            std::cerr << machine->getName() << " error: flag " << flag_name << " not found\n";
        else
            if (condition(machine))
                flag->setState("on");
            else
                flag->setState("off");
    }
    else if (!triggered && ( (trigger && trigger->fired()) || !trigger) && condition(machine)) {
        triggered = true;
        DBG_AUTOSTATES << machine->getName() << " subcondition triggered: " << command_name << " " << *(condition.predicate) << "\n";
        machine->execute(Message(command_name.c_str()),machine);
        if (trigger) trigger->disable();
    }
    else {
        DBG_AUTOSTATES <<"condition: " << (condition.predicate) << "\n";
        if (!trigger) { DBG_AUTOSTATES << "    condition does not have a timer\n"; }
        if (triggered) {DBG_AUTOSTATES <<"     condition " << (condition.predicate) << " already triggered\n";}
        if (condition(machine)) {DBG_AUTOSTATES <<"    condition " << (condition.predicate) << " passes\n";}
    }
    if (triggered) return true;
    return false;
}

void ConditionHandler::reset() {
    if (trigger) {
        //NB_MSG << "clearing trigger on subcondition of state " << s.state_name << "\n";
        if (trigger->enabled())
            trigger->disable();
        trigger->release();
    }
    trigger = 0;
    triggered = false;
}


std::ostream &operator<<(std::ostream &out, const Parameter &p) {
    return p.operator<<(out);
}

std::ostream &operator<<(std::ostream &out, const ActionTemplate &a) {
    return a.operator<<(out);
}

std::ostream &operator<<(std::ostream &out, const StableState &ss) {
    return ss.operator<<(out);
}

StableState::~StableState() {
    if (trigger) {
        if (trigger->enabled()) {
            trigger->disable();
            trigger->release();
        }
    }
    if (subcondition_handlers) {
        subcondition_handlers->clear();
        delete subcondition_handlers;
    }
}

StableState::StableState (const StableState &other)
    : state_name(other.state_name), condition(other.condition),
        uses_timer(other.uses_timer), timer_val(0), trigger(0), subcondition_handlers(0), owner(other.owner)
{
    if (other.subcondition_handlers) {
        subcondition_handlers = new std::list<ConditionHandler>;
        std::copy(other.subcondition_handlers->begin(), other.subcondition_handlers->end(), back_inserter(*subcondition_handlers));
    }
}


void StableState::triggerFired(Trigger *trig) {
    if (owner) owner->setNeedsCheck();
}


void StableState::refreshTimer() {
    // prepare a new trigger. note: very short timers will still be scheduled
    trigger = new Trigger("Timer");
    long trigger_time;

    // BUG here. If the timer comparison is '>' (ie Timer should be > the given value
    //   we should trigger at v.iValue+1
    if (timer_val.kind == Value::t_symbol) {
        Value v = owner->getValue(timer_val.sValue);
        if (v.kind != Value::t_integer) {
            NB_MSG << owner->getName() << " Error: timer value for state " << state_name << " is not numeric\n";
		NB_MSG << "timer_val: " << timer_val << " lookup value: " << v << " type: " << v.kind << "\n";
                return;
            }
            else
                trigger_time = v.iValue;
        }
        else if (timer_val.kind == Value::t_integer)
            trigger_time = timer_val.iValue;
        else {
            DBG_SCHEDULER << owner->getName() << " Warning: timer value for state " << state_name << " is not numeric\n";
		NB_MSG << "timer_val: " << timer_val << " type: " << timer_val.kind << "\n";
            return;
        }
        DBG_SCHEDULER << owner->getName() << " Scheduling timer for " << timer_val*1000 << "us\n";
        Scheduler::instance()->add(new ScheduledItem(trigger_time*1000, new FireTriggerAction(owner, trigger)));
}

std::map<std::string, MachineInstance*> machines;
std::map<std::string, MachineClass*> machine_classes;

// All machine instances automatically join and leave this list. 
// During the poll process, all machines in this list have their idle() called.
std::list<MachineInstance*> MachineInstance::all_machines;
std::list<MachineInstance*> MachineInstance::automatic_machines;
std::list<MachineInstance*> MachineInstance::active_machines;
std::map<std::string, HardwareAddress> MachineInstance::hw_names;


std::list<MachineClass*> MachineClass::all_machine_classes;

std::map<std::string, MachineClass> MachineClass::machine_classes;

/*
std::string fullName(const MachineInstance &m) {
	std::string name;
	if (m.owner) name = m.owner->getName() + ".";
	name += m.getName();
	return name;
}
*/
void MachineInstance::setNeedsCheck() {
    ++needs_check;
    if (_type == "LIST") {
        std::set<MachineInstance*>::iterator dep_iter = depends.begin();
        while (dep_iter != depends.end()) {
            MachineInstance *dep = *dep_iter++;
            dep->setNeedsCheck();
        }
    }
    else if (_type == "REFERENCE") {
        std::set<MachineInstance*>::iterator dep_iter = depends.begin();
        while (dep_iter != depends.end()) {
            MachineInstance *dep = *dep_iter++;
            dep->setNeedsCheck();
        }
    }
}

std::string MachineInstance::fullName() const {
    std::string res = _name;
    MachineInstance *o = owner;
    while (o) {
        res = o->getName() + "." + res;
        o = o->owner;
    }
    return res;
}


#if 0
void MachineInstance::add_io_entry(const char *name, unsigned int io_offset, unsigned int bit_offset){
	HardwareAddress addr(io_offset, bit_offset);
	addr.io_offset = io_offset;
	addr.io_bitpos = bit_offset;
	hw_names[name] = addr;
}
#endif

void Transmitter::send(Message *m, Receiver *r, bool expect_reply) {
	DBG_M_MESSAGING << _name << " message " << m->getText() << " expect reply: " << expect_reply << "\n";
    Package *p = new Package(this, r, m, expect_reply);
    Dispatcher::instance()->deliver(p);
}

bool TriggeredAction::active() {
    std::set<IOComponent*>::iterator iter = trigger_on.begin();
    while (iter != trigger_on.end()) {
        IOComponent *ioc = *iter++;
		if (!ioc->isOn())
			return false; 
	}
	iter = trigger_off.begin();
    while (iter != trigger_off.end()) {
        IOComponent *ioc = *iter++;
		if (!ioc->isOff())
			return false; 
	}
	return true;
}


void MachineInstance::triggerFired(Trigger *trig) {
    setNeedsCheck();
}


void MachineInstance::resetTemporaryStringStream() {
	ss.clear();
	ss.str("");
}

MachineInstance *MachineInstance::lookup(Parameter &param) {
	assert(param.val.kind == Value::t_symbol);
	if (!param.machine) {
		param.machine = lookup(param.real_name);
	}
	return param.machine;
}

MachineInstance *MachineInstance::lookup(Value &val) {
	assert(val.kind == Value::t_symbol);
    if (!val.cached_machine) {
		val.cached_machine = lookup(val.sValue);
    }
    return val.cached_machine;
}

bool MachineInstance::uses(MachineInstance *other) {
	if (_type == other->_type) return other->_name < _name;
	if (other->_type == "MODULE") return true;
	if (_type == "MODULE") return false;
	if (other->_type == "POINT") return true;
	if (other->_type == "ANALOGINPUT") return true;
	if (other->_type == "ANALOGOUTPUT") return true;
	if (other->_type == "STATUS_FLAG") return true;
	if (_type == "POINT") return false;
	if (_type == "ANALOGINPUT") return true;
	if (_type == "ANALOGOUTPUT") return true;
	if (_type == "STATUS_FLAG") return true;
	if (other->_type == "FLAG") return true;
	if (_type == "FLAG") return false;
	if (dependsOn(other)) return true;
	if (other->dependsOn(this)) return false;
	return other->_name < _name;
}

// return true if b depends on a
bool machine_dependencies( MachineInstance *a, MachineInstance *b) {
	return b->uses(a);
}

void MachineInstance::sort() {
	std::vector<MachineInstance*> tmp(all_machines.size());	
	std::copy(all_machines.begin(), all_machines.end(), tmp.begin());
    std::sort(tmp.begin(), tmp.end(), machine_dependencies);
	all_machines.clear();
	std::copy(tmp.begin(), tmp.end(), back_inserter(all_machines));
	
#if 0
	std::cout << "Sorted machines (dependencies)\n";
	BOOST_FOREACH(MachineInstance *m, all_machines) {
		if (m->owner) std::cout << m->owner->getName() << ".";
		std::cout << m->getName() << "\n";
	}
#endif
}

MachineInstance *MachineInstance::lookup(const char *name) {
	std::string seek_machine_name(name);
	return lookup(seek_machine_name);
}

MachineInstance *MachineInstance::lookup(const std::string &seek_machine_name)  {
	std::map<std::string, MachineInstance *>::iterator iter = localised_names.find(seek_machine_name);
	if (iter != localised_names.end()) {
		return (*iter).second;
	}
	return lookup_cache_miss(seek_machine_name);
}

MachineInstance *MachineInstance::lookup_cache_miss(const std::string &seek_machine_name)  {
	
	std::string short_name(seek_machine_name);
	size_t pos = short_name.find('.');
	if (pos != std::string::npos) {
		std::string machine_name(seek_machine_name);
		machine_name.erase(pos);
		std::string extra = short_name.substr(pos+1);
		MachineInstance *mi = lookup(machine_name);
		if (mi) 
			return mi->lookup(extra);
		else
			return 0;
	}

	MachineInstance *found = 0;
	resetTemporaryStringStream();
    //ss << _name<< " cache miss lookup " << seek_machine_name << " from " << _name;
    if (seek_machine_name == "SELF" || seek_machine_name == _name) { 
		//ss << "...self";
		if (_type == "CONDITION" && owner)
			found = owner;
		else
			found = this; 
		goto cache_local_name;
    }
	else if (seek_machine_name == "OWNER" ) {
		if (owner) found = owner;
		goto cache_local_name;
	}
    for (unsigned int i=0; i<locals.size(); ++i) {
        Parameter &p = locals[i];
        if (p.val.kind == Value::t_symbol && p.val.sValue == seek_machine_name) {
			//ss << "...local";
			found = p.machine; 
			goto cache_local_name;
        }
    }
    for (unsigned int i=0; i<parameters.size(); ++i) {
        Parameter &p = parameters[i];
        if (p.val.kind == Value::t_symbol &&
			(p.val.sValue == seek_machine_name || p.real_name == seek_machine_name) && p.machine) {
			//ss << "...parameter";
			found = p.machine; 
			goto cache_local_name;
        }
    }
    if (machines.count(seek_machine_name)) {
		//ss << "...global";
        found = machines[seek_machine_name];
		goto cache_local_name;
	}
    //ss << " not found";
//	DBG_M_MACHINELOOKUPS << ss << "";
	return 0;
	
	cache_local_name:
//	DBG_M_MACHINELOOKUPS << "\n";
	
	if (found) {
		if (seek_machine_name != found->getName())  {
			DBG_M_MACHINELOOKUPS << " linked " << seek_machine_name << " to " << found->getName() << "\n";
		}
		localised_names[seek_machine_name] = found;
	}
    return found;
}
std::ostream &operator<<(std::ostream &out, const MachineInstance &m) { return m.operator<<(out); }

/* Machine instances have different ways of reporting their 'current value', depending on the type of machine */

class MachineValue : public DynamicValue {
public:
    MachineValue(MachineInstance *mi): machine_instance(mi) { }
    Value operator()(MachineInstance *m)  {
        if (machine_instance->_type == "VARIABLE" || machine_instance->_type == "CONSTANT") {
            return machine_instance->getValue("VALUE");
        }
        else {
            return *machine_instance->getCurrentStateVal();
        }
    }
    DynamicValue *clone() const;
protected:
    MachineInstance *machine_instance;
};

class VariableValue : public DynamicValue {
public:
    VariableValue(MachineInstance *mi): machine_instance(mi) { }
    virtual Value operator()(MachineInstance *m) {
        if (machine_instance->_type == "VARIABLE" || machine_instance->_type == "CONSTANT") {
            return machine_instance->getValue("VALUE");
        }
        else {
            return machine_instance->getCurrentStateVal();
        }
    }
    DynamicValue *clone() const;
protected:
    MachineInstance *machine_instance;
};

DynamicValue *MachineValue::clone() const {
    MachineValue *mv = new MachineValue(*this);
    return mv;
}

DynamicValue *VariableValue::clone() const {
    VariableValue *vv = new VariableValue(*this);
    return vv;
}

MachineInstance::MachineInstance(InstanceType instance_type) 
        : Receiver(""), 
    _type("Undefined"), 
    io_interface(0), 
    owner(0), 
    needs_check(1),
    uses_timer(false),
    my_instance_type(instance_type),
    state_change(0), 
    state_machine(0),
    current_state("undefined"),
    is_enabled(false),
    locked(0),
    modbus_exported(none),
    saved_state("undefined"),
    current_state_val("undefined"),
    is_active(false)
{
    if (_type != "LIST" && _type != "REFERENCE")
        current_value_holder.setDynamicValue(new MachineValue(this));
	if (instance_type == MACHINE_INSTANCE) {
	    all_machines.push_back(this);
	    Dispatcher::instance()->addReceiver(this);
//		timer.tv_sec = 0;
//		timer.tv_usec = 0;
		gettimeofday(&start_time, 0);
	}
    if (!persistentStore) {
        DBG_PROPERTIES << "Initialising persistence feed\n";
        persistentStore = new MessagingInterface(1, persistent_store_port());
        usleep(200000); // give subscribers a chance to connect before publishing
    }
}

MachineInstance::MachineInstance(CStringHolder name, const char * type, InstanceType instance_type)
        : Receiver(name), 
    _type(type), 
    io_interface(0), 
    owner(0), 
    needs_check(1),
    uses_timer(false),
    my_instance_type(instance_type),
    state_change(0), 
    state_machine(0), 
    current_state("undefined"),
    is_enabled(false),
    locked(0),
    modbus_exported(none),
    saved_state("undefined"),
    current_state_val("undefined"),
    is_active(false),
    current_value_holder(0)
{
    if (_type != "LIST" && _type != "REFERENCE")
        current_value_holder.setDynamicValue(new MachineValue(this));
	if (instance_type == MACHINE_INSTANCE) {
	    all_machines.push_back(this);
	    Dispatcher::instance()->addReceiver(this);
//		timer.tv_sec = 0;
//		timer.tv_usec = 0;
		gettimeofday(&start_time, 0);
	}
    if (!persistentStore) {
        DBG_PROPERTIES << "Initialising persistence feed\n";
        persistentStore = new MessagingInterface(1, persistent_store_port());
        usleep(200000); // give subscribers a chance to connect before publishing
    }
}

MachineInstance::~MachineInstance() {
    all_machines.remove(this);
    automatic_machines.remove(this);
    active_machines.remove(this);
    Dispatcher::instance()->removeReceiver(this);
}


void MachineInstance::describe(std::ostream &out) {
    out << "---------------\n" << _name << ": " << current_state.getName() << " " << (enabled() ? "" : "DISABLED") <<  "\n"
		<< "  Class: " << _type << " instantiated at: " << definition_file << " line:" << definition_line << "\n";
    if (locked) {
		out << "Locked by: " << locked->getName() << "\n";
	}
    if (parameters.size()) {
        for (unsigned int i=0; i<parameters.size(); i++) {
            Value p_i = parameters[i].val;
            if (p_i.kind == Value::t_symbol) {
                out << "  parameter " << (i+1) << " " << p_i.sValue << " (" << parameters[i].real_name 
					<< "), state: "
					<< (parameters[i].machine ? parameters[i].machine->getCurrent().getName(): "") <<  "\n";
            }
			else {
                out << "  parameter " << (i+1) << " " << p_i << " (" << parameters[i].real_name << ")\n";
			}
        }
        out << "\n";
    }
	if (io_interface) out << " io: " << *io_interface << "\n";
    if (locals.size()) {
        out << "Locals:\n";
        for (unsigned int i = 0; i<locals.size(); ++i) {
			if (locals[i].machine)
            	out << "  " <<locals[i].val << " (" << locals[i].machine->getName()  << "):   " << locals[i].machine->getCurrent().getName() << "\n";
			else
	            out << locals[i].val <<"  Missing machine\n";
        }
    }
    if (listens.size()) {
        out << "Listening to: \n";
        std::set<Transmitter *>::iterator iter = listens.begin();
        while (iter != listens.end()) {
            Transmitter *t = *iter++;
			MachineInstance *machine = dynamic_cast<MachineInstance*>(t);
                if (machine) {
                    out << "  " << machine->getName() << "[" << machine->getId() << "]" << ":   " << (machine->enabled() ? machine->getCurrent().getName() : "DISABLED");
                if (machine->owner) out << " owner: " << (machine->owner ? machine->owner->getName() : "null");
                out << "\n";
            }
			else
				if (t) out << "  " << t->getName() << "\n";
			
        }
        out << "\n";
    }
    if (depends.size()) {
        out << "Dependant machines: \n";
        std::set<MachineInstance *>::iterator iter = depends.begin();
        while (iter != depends.end()) {
            MachineInstance *machine = *iter++;
            if (machine) {
                out << "  " << machine->getName() << "[" << machine->getId() << "]" << ":   " << (machine->enabled() ? machine->getCurrent().getName() : "DISABLED");
                if (machine->owner) out << " owner: " << (machine->owner ? machine->owner->getName() : "null");
                out << "\n";
            }
			else
				out << " NULL\n";
			
        }
        out << "\n";
    }
    if (!active_actions.empty()) {
        out << "active actions:\n";
        displayActive(out);
    }
    if (properties.size()) {
        out << "properties:\n  ";
        out << properties << "\n\n";
    }
    if (locked) {
        out << "locked: " << locked->getName() << "\n";
    }
    if (uses_timer)
        out << "Uses timer in stable state evaluation\n";
    const Value *current_timer_val = getTimerVal();
    out << "Timer: " << *current_timer_val << "\n";
    if (stable_states.size()) {
        out << "Last stable state evaluation:\n";
        for (unsigned int i=0; i<stable_states.size(); ++i) {
            out << "  " << stable_states[i].state_name << ": " << stable_states[i].condition.last_evaluation << "\n";
            if (stable_states[i].condition.last_result == true) {
                if (stable_states[i].subcondition_handlers && !stable_states[i].subcondition_handlers->empty()) {
                    std::list<ConditionHandler>::iterator iter = stable_states[i].subcondition_handlers->begin();
                    while (iter != stable_states[i].subcondition_handlers->end()) {
                        ConditionHandler &ch = *iter++;
                        out << "      " << ch.command_name <<" ";
                        if (ch.command_name != "FLAG" && ch.trigger) out << (ch.trigger->fired() ? "fired" : "not fired");
                        out << " last: " << ch.condition.last_evaluation << "\n";
                    }
                }
                break;
            }
        }
    }
    
}

void MachineInstance::listenTo(MachineInstance *m) {
    if (!listens.count(m)) {
        listens.insert(m);
        for (unsigned int ss_idx = 0; ss_idx < stable_states.size(); ++ss_idx) {
            StableState &s = stable_states[ss_idx];
            if (s.condition.predicate) s.condition.predicate->flushCache();
        }
        setNeedsCheck();
    }
}

void MachineInstance::stopListening(MachineInstance *m) {
    listens.erase(m);
    for (unsigned int ss_idx = 0; ss_idx < stable_states.size(); ++ss_idx) {
        StableState &s = stable_states[ss_idx];
        if (s.condition.predicate) s.condition.predicate->flushCache();
    }
    setNeedsCheck();
}

bool checkDepend(std::set<Transmitter*>&checked, Transmitter *seek, Transmitter *test) {
	if (seek == test) return true;
	MachineInstance *mi = dynamic_cast<MachineInstance*>(test);
	if (mi)	{ 
		BOOST_FOREACH(Transmitter *machine, mi->listens) {
			if (!checked.count(machine)) {
				checked.insert(machine);
				if (checkDepend(checked, seek, machine)) return true;
			}
		}
	}
	return false;
}

void MachineInstance::addDependancy(MachineInstance *m) { 
	if (m && m != this && !depends.count(m)) {
		depends.insert(m); 
		DBG_M_DEPENDANCIES << _name << " added dependant machine " << m->getName() << "\n";
	}
}

void MachineInstance::removeDependancy(MachineInstance *m) {
	if (m && m != this && depends.count(m)) {
        std::set<MachineInstance*>::iterator found = depends.find(m);
		depends.erase(found);
		DBG_M_DEPENDANCIES << _name << " removed dependant machine " << m->getName() << "\n";
	}
}


bool MachineInstance::dependsOn(Transmitter *m) {
	std::set<Transmitter*>checked;
	return checkDepend(checked, m, this);
}


bool MachineInstance::needsCheck() {
	return needs_check != 0;
}

void MachineInstance::idle() {
	if (error_state) {
		return;
	}
	if (!is_enabled) {
		return;
	}
    if (!state_machine) {
        std::stringstream ss;
        ss << " machine " << _name << " has no state machine";
        char *log_msg = strdup(ss.str().c_str());
        MessageLog::instance()->add(log_msg);
        DBG_M_ACTIONS << log_msg << "\n";
        free(log_msg);
        return;
    }

	Action *curr = executingCommand();
	while (curr) {
		curr->retain();
		if (curr->getStatus() == Action::New || curr->getStatus() == Action::NeedsRetry) {
            stop(curr); // avoid double queueing
			Action::Status res = (*curr)();
			if (res == Action::Failed) {
                std::stringstream ss; ss << _name << ": Action " << *curr << " failed: " << curr->error() << "\n";
                NB_MSG << ss.str();
                MessageLog::instance()->add(ss.str().c_str());
			}
            else if (res != Action::Complete) {
                DBG_M_ACTIONS << "Action " << *curr << " is not complete, waiting...\n";
				curr->release();
                return;
            }
		}
		else if (!curr->complete())  {
			//DBG_M_ACTIONS << "Action " << *curr << " is not still not complete\n";
			curr->release();
			return;
		}
		Action *last = curr;
		curr = executingCommand();
		if (curr && curr == last) {
			DBG_M_ACTIONS << "Action " << *curr << " failed to remove itself when complete. doing so manually\n";
			stop(curr);
			curr->release();
			curr = executingCommand();
			assert(curr != last);
		}
        else
            last->release();
	}
	while (!mail_queue.empty()){
		if (!mail_queue.empty()) {
            boost::mutex::scoped_lock(q_mutex);
			DBG_M_MESSAGING << _name << " has " <<  mail_queue.size() << " messages waiting\n";
			Package p = mail_queue.front();
			DBG_M_MESSAGING << _name << " found package " << p << "\n";
			mail_queue.pop_front();
			handle(p.message, p.transmitter, p.needs_receipt);
		}
	}
	return;
}

long get_diff_in_microsecs(struct timeval *now, struct timeval *then) {
    uint64_t t = (now->tv_sec - then->tv_sec);
    t = t * 1000000 + (now->tv_usec - then->tv_usec);
    return t;
}

// Machine idle processing
/*
 Each machine contains a timer that indicates how long the machine has been
 in a particular state. This polling loop first ensures that the timer value
 is updated for all machines and then calles idle() on each machine.
 */

const Value *MachineInstance::getTimerVal() {
    struct timeval now;
    gettimeofday(&now, NULL);
    long msecs = get_diff_in_microsecs(&now, &start_time)/1000;
	timer_val = msecs;
	if (debug()) {
		DBG_PREDICATES << _name << " TIMER is " << msecs << "\n";
	}
	return &timer_val;
}
void MachineInstance::processAll(PollType which) {
    bool builtins = false;
    if (which == BUILTINS)
		builtins = true;
	//MachineInstance::updateAllTimers(which);
    std::list<MachineInstance *>::iterator iter = active_machines.begin();
    while (iter != active_machines.end()) {
        MachineInstance *m = *iter++;
		if ( (builtins && m->_type == "POINT") || (!builtins && (m->_type != "POINT" || m->mq_interface)) ) {
			m->idle();
		}
	}
}

void MachineInstance::checkStableStates() {
    std::list<MachineInstance *>::iterator iter = MachineInstance::automatic_machines.begin();
    while (iter != MachineInstance::automatic_machines.end()) {
        MachineInstance *m = *iter++;
		if ( m->enabled() && m->executingCommand() == NULL
                && (m->needsCheck() || m->_type == "CONDITION" ) )
			m->setStableState();
	}
}

void MachineInstance::displayAutomaticMachines() {
	BOOST_FOREACH(MachineInstance *m, MachineInstance::automatic_machines) {
		std::cout << *m << "\n";
	}
}

void MachineInstance::displayAll() {
	BOOST_FOREACH(MachineInstance *m, all_machines) {
		std::cout << "-------" << m->getName() << "\n";
		std::cout << *m << "\n";
	}
}


// linear search through the machine list, tbd performance..
MachineInstance *MachineInstance::find(const char *name) {
    std::string machine(name);
    if (machine.find('.') != std::string::npos) {
        machine.erase(machine.find('.'));
        MachineInstance *mi = find(machine.c_str());
        if (mi) {
            std::string shortname(name);
            shortname = shortname.substr(shortname.find('.')+1);
            return mi->lookup(shortname);
        }
    }
    else {
        std::list<MachineInstance*>::iterator iter = all_machines.begin();
        while (iter != all_machines.end()) {
            MachineInstance *m = *iter++;
            if (m->_name == name) return m;
        }
    }
	return 0;
}

void MachineInstance::addParameter(Value param, MachineInstance *mi) {
    parameters.push_back(param);
    parameters[parameters.size()-1].machine = mi;
    if (!mi && param.kind == Value::t_symbol) {
        mi = lookup(param);
        if (mi && !param.cached_machine)
            param.cached_machine = mi;
    }
    if (mi) addDependancy(mi);
    if (_type == "LIST") {
        setNeedsCheck();
    }
}

void MachineInstance::addLocal(Value param, MachineInstance *mi) {
    locals.push_back(param);
    locals[locals.size()-1].machine = mi;
    if (!mi && param.kind == Value::t_symbol) mi = lookup(param.asString().c_str());
    std::set<MachineInstance*>::iterator dep_iter = depends.begin();
    while (dep_iter != depends.end()) {
        MachineInstance *dep = *dep_iter++;
        if (mi) {
            mi->addDependancy(dep);
            dep->listenTo(mi);
        }
        dep->setNeedsCheck();
    }
    
    if (mi) {
        mi->addDependancy(this); listenTo(mi);
        for (unsigned int ss_idx = 0; ss_idx < stable_states.size(); ++ss_idx) {
            StableState &s = stable_states[ss_idx];
            if (s.condition.predicate) s.condition.predicate->flushCache();
        }
    }
    if (_type == "REFERENCE") {
        setNeedsCheck();
        /*if (owner) {
            std::set<MachineInstance*>::iterator dep_iter = owner->depends.begin();
            while (dep_iter != owner->depends.end()) {
                MachineInstance *dep = *dep_iter++;
                if (mi){
                    mi->addDependancy(dep);
                    dep->listenTo(mi);
                }
                dep->setNeedsCheck();
            }
        }*/
    }
}

void MachineInstance::removeLocal(int index) {
    Parameter &p = locals[0];
    if (p.machine) {
        stopListening(p.machine);
        p.machine->removeDependancy(this);
    }
    // each dependency of the reference should no longer be dependant on changees to the entry
    std::set<MachineInstance*>::iterator dep_iter =depends.begin();
    if (p.machine)
        while (dep_iter != depends.end()) {
            MachineInstance *dep = *dep_iter++;
            p.machine->removeDependancy(dep);
            dep->stopListening(p.machine);
            dep->setNeedsCheck();
        }
    std::vector<Parameter>::iterator iter = locals.begin();
    while (index-- && iter != locals.end()) iter++;
    if (iter != locals.end()) locals.erase(iter);
    setNeedsCheck();
}

void MachineInstance::setProperties(const SymbolTable &props) {
    properties.clear();
    SymbolTableConstIterator iter = props.begin();
    while (iter != props.end()) properties.push_back(*iter++);
}

void MachineInstance::setDefinitionLocation(const char *file, int line_no) {
    definition_file = file;
    definition_line = line_no;
}

//void addReferenceLocation(const char *file, int line_no);
MachineInstance &MachineInstance::operator=(const MachineInstance &orig) {
    id = orig.id;
    _name = orig._name;
    _type = orig._type;
    parameters.clear();
    definition_file = orig.definition_file;
    definition_line = orig.definition_line;
	uses_timer = orig.uses_timer;
    std::copy(orig.parameters.begin(), orig.parameters.end(), std::back_inserter(parameters));
    std::copy(orig.locals.begin(), orig.locals.end(), std::back_inserter(locals));
    return *this;
}

std::ostream &MachineInstance::operator<<(std::ostream &out)const  {
    out << _name << "(" << id <<")" << "<" << _type << ">\n";
    if (properties.begin() != properties.end()) {
        out << "  Properties: " << properties << "\n"; 
    }
	if (!parameters.empty()) {
		for (unsigned int i=0; i<parameters.size(); ++i) {
	        out << "  Parameter " << i << ": " << parameters[i].val << " real name: " << parameters[i].real_name << " ";
	        if (!parameters[i].properties.empty())
	            out << "(" << parameters[i].properties << ")";
			MachineInstance *mi = parameters[i].machine;
			if (mi) out << "=>" << mi->getName()<< ":"<< mi->id<<" ";
			out << "\n";
	    }
	}
	if (!locals.empty()) {
		for (unsigned int i=0; i<locals.size(); ++i) {
	        out << "  Local " << i << " " << locals[i].val;
	        if (!locals[i].properties.empty())
	            out << "(" << locals[i].properties << ") ";
			MachineInstance *mi = locals[i].machine;
			if (mi) out << "=>" << (mi->getName())<< ":"<< mi->id<<" ";
	    }
		out << "\n";
	}
    if (uses_timer) out << "Uses Timer\n";
	if (!depends.empty()) {
	    out << "Dependant machines: ";
	    BOOST_FOREACH(MachineInstance *dep, depends) out << dep->getName() << ",";
		out << "\n";
	}
    out << "  Defined at: " << definition_file << ":" << definition_line << "\n";
    return out;
}

bool MachineInstance::stateExists(State &seek) {
    std::list<State>::const_iterator iter = state_machine->states.begin();
    while (iter != state_machine->states.end()) {
        if (*iter == seek) return true;
        iter++;
    }
    return false;
}

/*
	Machines hear messages only when registered to do so, the rules are:
	* all machines receive messages from themselves
	* machines receive messages from objects they are listening to
	* commands in the transition table are public and can be sent by anyone
 */
bool MachineInstance::receives(const Message&m, Transmitter *from) {
    // passive machines do not receive messages
    if (!is_active) return false;
    // all active machines receive messages from themselves
    if (from == this) {
        DBG_M_MESSAGING << "Machine " << getName() << " receiving " << m << " from itself\n";
        return true; 
    }    
    // machines receive messages from objects they are listening to
    if (std::find(listens.begin(), listens.end(), from) != listens.end()) {
        DBG_M_MESSAGING << "Machine " << getName() << " receiving " << m << " from " << from->getName() << "\n";
        return true;
    }
    if (commands.count(m.getText()) != 0) {
        return true;    // items in the command table are public and can be sent by anyone
    }

	// commands in the transition table are public and can be sent by anyone
    std::list<Transition>::const_iterator iter = transitions.begin();
    while (iter != transitions.end()) {
        const Transition &t = *iter++;
        if (t.trigger == m) return true;
    }
    DBG_M_MESSAGING << "Machine " << getName() << " ignoring " << m << " from " << ((from) ? from->getName() : "NULL") << "\n";
    return false;
}

Action::Status MachineInstance::setState(State new_state, bool reexecute) {
	Action::Status stat = Action::Complete; 
	// update the Modbus interface for self 
	if (modbus_exported == discrete || modbus_exported == coil) {
		if (!modbus_exports.count(_name))
			DBG_M_MSG << _name << " Error: modbus export info not found\n";
		else {
			ModbusAddress ma = modbus_exports[_name];
			if (ma.getSource() == ModbusAddress::machine && ( ma.getGroup() != ModbusAddress::none) ) {
				if (new_state.getName() == "on") { // just turned on
					DBG_MODBUS << _name << " machine came on; triggering a modbus message\n";
					ma.update(1);
				}
				else if (current_state.getName() == "on") {
					DBG_MODBUS << _name << " machine went off; triggering a modbus message\n";
					ma.update(0);
				}		
			}
			else {
				DBG_M_MSG << _name << ": exported: " << modbus_exported << " (" << ma << ") but address is of type 'none'\n";
			}
		}
	}
    assert(reexecute == false);
	if (reexecute || current_state != new_state) {
		std::string last = current_state.getName();

#ifndef DISABLE_LEAVE_FUNCTIONS
        /* notify everyone we are leaving a state */
        {
            std::string txt = _name + "." + current_state.getName() + "_leave";
            Message msg(txt.c_str());
            stat = execute(msg, this);
            
            std::set<MachineInstance*>::iterator dep_iter = depends.begin();
            while (dep_iter != depends.end()) {
                MachineInstance *dep = *dep_iter++;
                if (this == dep) continue;
                if (!dep->receives(msg, this)) continue;
                Action *act = dep->executingCommand();
                if (act) {
                    if (act->getStatus() == Action::Suspended) act->resume();
                    DBG_M_MESSAGING << dep->getName() << "[" << dep->getCurrent().getName() << "]" << " is executing " << *act << "\n";
                }
                //TBD execute the message on the dependant machine
                dep->execute(msg, this);
                if (dep->_type == "LIST") {
                    dep->setNeedsCheck();
                }
            }
        }
#endif
		gettimeofday(&start_time,0);
		gettimeofday(&disabled_time,0);
        //DBG_MSG << fullName() << " changing from " << current_state << " to " << new_state << "\n";
		current_state = new_state;
		current_state_val = new_state.getName();
		properties.add("STATE", current_state.getName().c_str(), SymbolTable::ST_REPLACE);
        // publish the state change if we are a publisher
        if (mq_interface) {
            if (_type == "POINT" && properties.lookup("type") == "Output") {
                mq_interface->publish(properties.lookup("topic").asString(), new_state.getName(), this);
            }
        }
		// update the Modbus interface for the state
		if (modbus_exports.count(_name + "." + last)){
			ModbusAddress ma = modbus_exports[_name + "." + last];
			DBG_MODBUS << _name << " leaving state " << last << " triggering a modbus message " << ma << "\n";
			assert(ma.getSource() == ModbusAddress::state);
			ma.update(0);
		}
		if (modbus_exports.count(_name + "." + current_state.getName())){
			ModbusAddress ma = modbus_exports[_name + "." + current_state.getName()];
			DBG_MODBUS << _name << " active state " << current_state.getName() << " triggering a modbus message " << ma << "\n";
			assert(ma.getSource() == ModbusAddress::state);
			ma.update(1);
		}
        /* TBD optimise the timer triggers to only schedule the earliest trigger
        StableState *earliestTimerState = NULL;
        unsigned long ealiestTimer = (unsigned long) -1L;
         */
		
        for (unsigned int ss_idx = 0; ss_idx < stable_states.size(); ++ss_idx) {
            StableState &s = stable_states[ss_idx];
			if (s.uses_timer) {
				long timer_val;
				// first disable any trigger that may still be enabled
				if ( s.trigger) {
                    //NB_MSG << "clearing trigger for state " << s.state_name << "\n";
                    if (s.trigger->enabled() ) {
                        DBG_M_SCHEDULER << _name << " disabling " << s.trigger->getName() << "\n";
                        s.trigger->disable();
                    }
                    s.trigger->release();
				}
                s.condition.predicate->clearTimerEvents(this);

                // BUG here. If the timer comparison is '>' (ie Timer should be > the given value
                //   we should trigger at v.iValue+1
				if (s.timer_val.kind == Value::t_symbol) {
					Value v = getValue(s.timer_val.sValue);
					if (v.kind != Value::t_integer) {
						NB_MSG << _name << " Error: timer value for state " << s.state_name << " is not numeric\n";
						NB_MSG << "timer_val: " << s.timer_val << " lookup value: " << v << " type: " << v.kind << "\n";
						continue;
					}
					else
						timer_val = v.iValue;
				}
				else if (s.timer_val.kind == Value::t_integer)
					timer_val = s.timer_val.iValue;
				else {
					DBG_M_SCHEDULER << _name << " Warning: timer value for state " << s.state_name << " is not numeric\n";
					NB_MSG << "timer_val: " << s.timer_val << " type: " << s.timer_val.kind << "\n";
					continue;
				}
                if (s.condition.predicate->op == opGT) timer_val++;
                else if (s.condition.predicate->op == opLT) --timer_val;
				DBG_M_SCHEDULER << _name << " Scheduling timer for " << timer_val*1000 << "us\n";
				// prepare a new trigger. note: very short timers will still be scheduled
                // TBD move this outside of the loop and only apply it for the earliest timer
				s.trigger = new Trigger("Timer");
				Scheduler::instance()->add(new ScheduledItem(timer_val*1000, new FireTriggerAction(this, s.trigger)));
			}
			if (s.subcondition_handlers) 
			{
                std::list<ConditionHandler>::iterator iter = s.subcondition_handlers->begin();
                while (iter != s.subcondition_handlers->end()) {
                    ConditionHandler &ch = *iter++;
					//setup triggers for subcondition handlers

					if (ch.uses_timer) {
						long timer_val;
                        ch.reset();
						if (s.state_name == current_state.getName()) {
                            // BUG here. If the timer comparison is '>' (ie Timer should be > the given value
                            //   we should trigger at v.iValue+1
                            //NB_MSG << "setting up trigger for subcondition on state " << s.state_name << "\n";

                            std::list<Predicate *> timer_clauses;
                            DBG_PREDICATES << "searcing: " << (*ch.condition.predicate) << "\n";
                            ch.condition.predicate->findTimerClauses(timer_clauses);
                            std::list<Predicate *>::iterator iter = timer_clauses.begin();
                            timer_val = LONG_MAX;
                            while (iter != timer_clauses.end()) {
                                Predicate *node = *iter++;
                                Value &tv = node->getTimerValue();
                                if (tv == SymbolTable::Null) continue;
                                if (tv.kind == Value::t_symbol || tv.kind == Value::t_string) {
                                    Value v = getValue(tv.sValue);
                                    if (v.kind != Value::t_integer) {
                                        DBG_MSG << _name << " Warning: timer value "<< v << " for state " 
                                            << s.state_name << " subcondition is not numeric\n";
                                        continue;
                                    }
                                    
                                    if (v.iValue>=0 && v.iValue < timer_val) {
                                        timer_val = v.iValue;
                                        if (node->op == opGT) ++timer_val;
                                    }
                                    else {
                                        DBG_PREDICATES << "skipping large timer value " << v.iValue << "\n";
                                    }
                                }
                                else if (tv.kind == Value::t_integer) {
                                    if (tv.iValue < timer_val) {
                                        timer_val = tv.iValue;
                                        if (node->op == opGT) ++timer_val;
                                    }
                                    else {
                                        DBG_PREDICATES << "skipping a large timer value " << tv.iValue << "\n";
                                    }
                                }
                                else {
                                    DBG_MSG<< _name << " Warning: timer value for state " << s.state_name << " subcondition is not numeric\n";
                                    continue;
                                }
                            }
                            if (timer_val < LONG_MAX) {
                                ch.timer_val = timer_val;
                                DBG_MSG << "minimum timer: " << timer_val << " selected\n";
                                DBG_M_SCHEDULER << _name << " Scheduling subcondition timer for " << timer_val*1000 << "us\n";
                                ch.trigger = new Trigger("Timer");
                                Scheduler::instance()->add(new ScheduledItem(timer_val*1000, new FireTriggerAction(this, ch.trigger)));
                            }
                            
/*
							if (ch.timer_val.kind == Value::t_symbol) {
								Value v = getValue(ch.timer_val.sValue);
								if (v.kind != Value::t_integer) {
									DBG_M_SCHEDULER << _name << " Warning: timer value for state " << s.state_name << " subcondition is not numeric\n";
									continue;
								}
								else
									timer_val = v.iValue;
							}
							else if (ch.timer_val.kind == Value::t_integer)
								timer_val = ch.timer_val.iValue;
							else {
								DBG_M_SCHEDULER << _name << " Warning: timer value for state " << s.state_name << " subcondition is not numeric\n";
								continue;
							}
							DBG_M_SCHEDULER << _name << " Scheduling subcondition timer for " << timer_val*1000 << "us\n";
							ch.trigger = new Trigger("Timer");
							Scheduler::instance()->add(new ScheduledItem(timer_val*1000, new FireTriggerAction(this, ch.trigger)));
 */
						}
					}
					else if (ch.command_name == "FLAG") {
						if (s.state_name == current_state.getName()) {
							//TBD What was intended here?
						}
					}
				}
			}
		}
        
		
		{
	        MessagingInterface *mif = MessagingInterface::getCurrent();
			resetTemporaryStringStream();
			if (owner) ss << owner->getName() << ".";
	        ss << fullName() << " " << " STATE " << new_state << std::flush;
	        //mif->send(ss.str().c_str());
            mif->sendState("STATE", fullName(), new_state.getName());
		}
        
        /* notify everyone we are entering a state */

		std::string txt = _name + "." + new_state.getName() + "_enter";
		Message msg(txt.c_str());
		stat = execute(msg, this);
        notifyDependents(msg);
	}
	return stat;
}

void MachineInstance::notifyDependents(Message &msg){
    std::set<MachineInstance*>::iterator dep_iter = depends.begin();
    while (dep_iter != depends.end()) {
        MachineInstance *dep = *dep_iter++;
        if (this == dep) continue;
        DBG_M_MESSAGING << _name << " should tell " << dep->getName() << " it is " << current_state.getName() << " using " << msg << "\n";
        Action *act = dep->executingCommand();
        if (act) {
            if (act->getStatus() == Action::Suspended) act->resume();
            DBG_M_MESSAGING << dep->getName() << "[" << dep->getCurrent().getName() << "]" << " is executing " << *act << "\n";
        }
        //TBD execute the message on the dependant machine
        dep->execute(msg, this);
        if (dep->_type == "LIST") {
            dep->setNeedsCheck();
        }
    }
}


/* findHandler - find a handler for a message by looking through the transition table 

    if the message not in dot-form (i.e., does not contain a dot), we search the transition
	  table for a matching transition

		- find a matching transition
		- if the transition is to a stable state
			- clear the trigger on the condition handlers for the state
			- construct a stable state IF test and push it to the action stack
			- if a response is required push an ExecuteMessage action  for the _done message
		- otherwise, 
			-push a move state action
			- if there is a matching command (unnecessary test) and the command is exported turn the command coil off

   If the message is in dot-form (i.e., includes a machine name), we treat is as a simple
	message :
		- do not execute any command linked to the transition
		- do not check requirements
		- push a move state action

*/

Action *MachineInstance::findHandler(Message&m, Transmitter *from, bool response_required) {
	std::string short_name(m.getText());
	if (short_name.find('.') != std::string::npos) {
		short_name = short_name.substr(short_name.find('.')+1);
	}
	if (from) {
		DBG_M_MESSAGING << "received message " << m << " from " << from->getName() << "\n";
	}
	if (from == this || m.getText() == short_name) {
	    std::list<Transition>::const_iterator iter = transitions.begin();
		while (iter != transitions.end()) {
   		 	const Transition &t = *iter++;
   		 	if (t.trigger.getText() == short_name && (current_state == t.source || t.source == "ANY")
                && (!t.condition || t.condition->operator()(this))) {
   	        	// found match, if there is a command defined for this transition, we
   	        	// execute the command, otherwise we just do the state change
			   	if (commands.count(t.trigger.getText())) {
					DBG_M_MESSAGING << "Transition on machine " << getName() << " has a linked command; using it\n";
					// at the end of the transition, we expect to have moved to the designated state
					// so we ensure that happens by pushing a state change
					// but first, we check the stablestate condition for that state.
                    
					// find state condition
					bool found = false;
                    for (unsigned int ss_idx = 0; ss_idx < stable_states.size(); ++ss_idx) {
                        StableState &s = stable_states[ss_idx];
						if (s.state_name == t.dest.getName()) {
							DBG_M_STATECHANGES << "found stable state condition test for " << t.dest.getName() << "\n";
							if (s.subcondition_handlers) {
                                std::list<ConditionHandler>::iterator iter = s.subcondition_handlers->begin();
                                while (iter != s.subcondition_handlers->end()) {
                                    ConditionHandler&ch = *iter++;
									ch.triggered = false;
								}
							}
							MoveStateActionTemplate temp(_name.c_str(), t.dest.getName().c_str() );
						    MachineCommandTemplate mc("stable_state_test", "");
						    mc.setActionTemplate(&temp);
							IfCommandActionTemplate ifcat(s.condition.predicate, &mc);
							this->push(new IfCommandAction(this, &ifcat));
							found = true;
						}
					}
					// the CALL method waits for a response once the executed command is complete
					// the response will be sent after the transition to the next state is done
					if (response_required && from) {
						DBG_M_MESSAGING << _name << " command " << t.trigger.getText() << " completion requires response\n";
						std::string response = _name + "." + t.trigger.getText() + "_done";
						ExecuteMessageActionTemplate emat(strdup(response.c_str()), strdup(from->getName().c_str()));
						ExecuteMessageAction *ema = new ExecuteMessageAction(this, emat);
						this->push(ema);
					}
					if (!found) {
						DBG_M_STATECHANGES << "no stable state condition test for " << t.dest.getName() << " pushing state change\n";
						MoveStateActionTemplate msat("SELF", t.dest);
						MoveStateAction *msa = new MoveStateAction(this, msat);
						DBG_M_STATECHANGES << _name << " pushed (status: " << msa->getStatus() << ") " << *msa << "\n";
						this->push(msa);
					}
					if (commands.count(t.trigger.getText()) ) {
						// if a modbus command, make sure it is turned off again in modbus
						if (modbus_exports.count(_name + "." + t.trigger.getText())) {
							ModbusAddress addr = modbus_exports[_name + "." + t.trigger.getText()];
							if (addr.getSource() == ModbusAddress::command) {
								DBG_MODBUS << _name << " turning off command coil " << addr << "\n";
								addr.update(0);
							}
							else {
								NB_MSG << _name << " command " << t.trigger.getText() << " is linked to an improper address\n";
							}
						}
						else {
							DBG_M_MODBUS << _name << " " << t.trigger.getText() << " is not exported to modbus, no response sent\n";
						}
						return commands[t.trigger.getText()]->retain();
					}
					else
						return NULL;
				}
				else {
				 	DBG_M_MESSAGING << "No linked command for the transition, performing state change\n";
                }

				// there is no matching command but a completion reply has been requested
                if (response_required && from) {
                    DBG_M_MESSAGING << _name << " command " << t.trigger.getText() << " completion requires response\n";
                    std::string response = _name + "." + t.trigger.getText() + "_done";
                    ExecuteMessageActionTemplate emat(strdup(response.c_str()), strdup(from->getName().c_str()));
                    ExecuteMessageAction *ema = new ExecuteMessageAction(this, emat);
                    this->push(ema);
                }

				// no matching command, just perform the transition
				MoveStateActionTemplate temp(strdup(_name.c_str()), strdup(t.dest.getName().c_str()) );
				return new MoveStateAction(this, temp);
			}
		}
		// no transition but this may still be a command
		DBG_M_MESSAGING << _name << " looking for a command with name " << short_name << "\n";
		if (commands.count(short_name)) {
			if (response_required && from) {
				DBG_M_MESSAGING << _name << " command " << short_name << " completion requires response\n";
				std::string response = _name + "." + short_name + "_done";
				ExecuteMessageActionTemplate emat(strdup(response.c_str()), strdup(from->getName().c_str()));
				ExecuteMessageAction *ema = new ExecuteMessageAction(this, emat);
				this->push(ema);
			}
			return commands[short_name]->retain();
		}
	}
	else {
	   	std::list<Transition>::const_iterator iter = transitions.begin();
		while (iter != transitions.end()) {
   	 		const Transition &t = *iter++;
   	 		if ( (t.trigger.getText() == m.getText() || t.trigger.getText() == short_name) && current_state == t.source) {
				DBG_M_MESSAGING << _name << " received message" << m.getText() << "; pushing state change\n";
				MoveStateActionTemplate msat("SELF", t.dest);
				MoveStateAction *msa = new MoveStateAction(this, msat);
				DBG_M_STATECHANGES << _name << " pushed (status: " << msa->getStatus() << ") " << *msa << "\n";
				return msa;
			}
		}
	}
    // if debugging, generate a log message for the enter function
    if (debug() && _type != "POINT" && LogState::instance()->includes(DebugExtra::instance()->DEBUG_ACTIONS) ) {
		std::string message_str = _name + "." + current_state.getName() + "_enter";
		if (message_str != m.getText())
			DBG_M_MESSAGING << _name << ": No transition found to handle " << m << " from " << current_state.getName() << ". Searching receive_functions for a handler...\n";
		else
			DBG_M_MESSAGING << _name << ": Looking for ENTER method for " << current_state.getName() << "\n";
	}
    std::map<Message, MachineCommand*>::iterator receive_handler_i = receives_functions.find(Message(m.getText().c_str()));
    if (receive_handler_i != receives_functions.end()) {
		if (debug()) {
			DBG_M_MESSAGING << " found event receive handler: " << (*receive_handler_i).first << "\n"
				<< "handler: " << *((*receive_handler_i).second) << "\n";
		}
        return (*receive_handler_i).second->retain();
	}
	else if (from == this) {
		if (short_name == m.getText()) { return NULL; } // no other alternatives
	    std::map<Message, MachineCommand*>::iterator receive_handler_i = receives_functions.find(Message(short_name.c_str()));
		if (receive_handler_i != receives_functions.end()) {
				if (debug()){
					DBG_M_MESSAGING << " found event receive handler: " << (*receive_handler_i).first << "\n"
						<< "handler: " << *((*receive_handler_i).second) << "\n";
				}
	       		return (*receive_handler_i).second->retain();
		}
	}
	return NULL;
}

void MachineInstance::collect(const Package &package) {
	if (package.transmitter) {
		if (!receives(package.message, package.transmitter)) return;
		if (package.transmitter)
			DBG_M_MESSAGING << _name << "Collecting package: " << package.transmitter->getName() << "." << *package.message << "\n";
		else
			DBG_M_MESSAGING << _name << "Collecting package: " << *package.message << "\n";
		HandleMessageActionTemplate hmat(package);
		HandleMessageAction *hma = new HandleMessageAction(this, hmat);
		Action *curr = executingCommand();
		if ( curr ) {
			curr->suspend();
			DBG_M_MESSAGING <<_name<< "Interrupted " << *curr << "\n";
		}
		DBG_M_MESSAGING << _name << " pushed " << *hma << "\n";
		active_actions.push_back(hma);
	}
}

Action::Status MachineInstance::execute(const Message&m, Transmitter *from) {
	if (!enabled()) {
#if 0
		if (from) {
			DBG_MESSAGING << _name << " dropped message " << m << " from " << from->getName() << " while disabled\n";
		}
		else {
			DBG_MESSAGING << _name << " dropped message " << m << " while disabled\n";
		}
#endif
		return Action::Failed;
	}

	if (m.getText().length() == 0) {
		NB_MSG << _name << " error: dropping empty message\n";
		return Action::Failed;
	}
	setNeedsCheck(); // TBD is this necessary?
	DBG_M_MESSAGING << _name << " executing message " << m.getText()  << " from " << ( (from) ? from->getName(): "unknown" ) << "\n";
	std::string event_name(m.getText());
	if (from && event_name.find('.') == std::string::npos)
		event_name = from->getName() + "." + m.getText();
    
    if (_type == "POINT" || _type == "SUBSCRIBER" || _type == "PUBLISHER" 
			|| _type == "ANALOGINPUT"  || _type == "ANALOGOUTPUT" || _type == "STATUS_FLAG" ) {
        if ( (io_interface && from == io_interface) || (mq_interface && from == mq_interface) ) {
            //std::string state_name = m.getText();
            //if (state_name.find('_') != std::string::npos)
            //    state_name = state_name.substr(state_name.find('_'));
            if(m.getText() == "on_enter" && current_state.getName() != "on") {
                setState("on");
            }
            else if (m.getText() == "off_enter" && current_state.getName() != "off") {
                setState("off");
            }
            else if (m.getText() == "property_change" ) {
                setValue("VALUE", io_interface->address.value);
            }
            // a POINT won't have an actions that depend on triggers so it's safe to return now
            return Action::Complete;
        }
    }

    // fire the triggers on any suspended commands that might be waiting for them.
	if (executingCommand()) {
		// tell any actions that are triggering on this that we have seen the event
		DBG_M_MESSAGING << _name << " processing triggers for " << event_name << "\n";
        std::list<Action*>::iterator iter = active_actions.begin();
        while (iter != active_actions.end()) {
            Action *a = *iter++;
			Trigger *trigger = a->getTrigger();
			if (trigger && trigger->matches(m.getText())) {
				SetStateAction *ssa = dynamic_cast<SetStateAction*>(a);
				CallMethodAction *cma = dynamic_cast<CallMethodAction*>(a);
				if (ssa) {
					if (!ssa->condition.predicate || ssa->condition(this)) {
						trigger->fire();
						DBG_M_MESSAGING << _name << " trigger on " << *a << " fired\n";
					}
					else
						DBG_MESSAGING << _name << " trigger message " << trigger->getName()
							<< " arrived but condition " << *(ssa->condition.predicate) << " failed\n";
				}
				// a call method action may be waiting for this message
				else if ( cma ) {
					if (cma->getTrigger()) cma->getTrigger()->fire();
					DBG_M_MESSAGING << _name << " trigger on " << *a << " fired\n";
				}
				else {
					NB_MSG << _name << " firing unknown trigger on " << *a << "\n";
					trigger->fire();
				}
			}
		}
	}
	// execute the method if there is one
	DBG_M_MESSAGING << _name<< " executing " << event_name << "\n";
	HandleMessageActionTemplate hmat(Package(from, this, new Message(event_name.c_str())));
	HandleMessageAction *hma = new HandleMessageAction(this, hmat);
    Action::Status res =  (*hma)();
	hma->release();
    return res;
}

void MachineInstance::handle(const Message&m, Transmitter *from, bool send_receipt) {
	if (!enabled()) {
	    DBG_M_MESSAGING << _name << ":" << id 
			<< " dropped: " << m << " in " << getCurrent() << " from " << from->getName() << " while disabled\n";
		if (send_receipt) {
			NB_MSG << _name << " Error: message '" << m.getText() << "' requiring a receipt arrived while disabled\n";
		}
		return;
	}
    if (_type != "POINT") {
	    DBG_M_MESSAGING << _name << ":" << id 
			<< " received: " << m << " in " << getCurrent() << " from " << from->getName() 
			<< " will send receipt: " << send_receipt << "\n";
	}
	HandleMessageActionTemplate hmat(Package(from, this, m, send_receipt));
	HandleMessageAction *hma = new HandleMessageAction(this, hmat);
	active_actions.push_front(hma);
}

MachineClass::MachineClass(const char *class_name) : default_state("unknown"), initial_state("INIT"), name(class_name), allow_auto_states(true) {
    all_machine_classes.push_back(this);
}

void MachineClass::defaultState(State state) {
    default_state = state;
}

bool MachineClass::isStableState(State &state) {
	std::vector<StableState>::iterator iter = stable_states.begin();
	while (iter != stable_states.end()) {
		StableState &s = *iter++;
		if (s.state_name == state.getName()) return true;
	}
	return false;
}


Action *MachineInstance::executingCommand() {
	if (!active_actions.empty()) return active_actions.back();
	return 0;
}

void MachineInstance::start(Action *a) {
	if (!active_actions.empty()) {
		Action *b = executingCommand();
		if (b && b->isBlocked() && b->blocker() != a) {
			DBG_M_ACTIONS << "WARNING: "<<_name << " is executing command " <<*b <<" when starting " << *a << "\n";
		}
		if (b == a) { return; }
	}
	DBG_M_ACTIONS << _name << " pushing (state: " << a->getStatus() << ")" << *a << "\n";
//    std::cout << "STARTING: " << *a << "\n";
	active_actions.push_back(a->retain());
}

void MachineInstance::displayActive(std::ostream &note) {
	if (active_actions.empty()) return;
	note << _name << ':' << id << " stack: ";
	const char *delim = "";
	BOOST_FOREACH(Action *act, active_actions) {
		note << delim << " " << *act << " (status=" << act->getStatus() << ")";
		delim = ", ";
	}
}

// stop removes the action
void MachineInstance::stop(Action *a) { 
	setNeedsCheck();
//	if (active_actions.back() != a) {
//		DBG_M_ACTIONS << _name << "Top of action stack is no longer " << *a << "\n";
//		return;
//	}
//	active_actions.pop_back();
//    a->release();
    
    bool found = false;
    std::list<Action*>::iterator iter = active_actions.begin();
    while (iter != active_actions.end()) {
        Action *queued = *iter;
        if (queued == a) {
            if (found)
                std::cerr << "Warning: action " << *a << " was queued twice\n";
            iter = active_actions.erase(iter);
            a->release();
            found = true;
			continue;
        }
        iter++;
    }
    
	if (!active_actions.empty()) {
		Action *next = active_actions.back();
		if (next) {
			next->setBlocker(0);
			if (next->suspended()) next->resume();
		}
	}
}

void Action::suspend() {
	if (status == Suspended) return;
	if (status != Running) {
		DBG_M_ACTIONS << owner->getName() << " suspend called when action is in state " << status << "\n";
	}
	saved_status = status;
	status = Suspended;
}

void Action::resume() {
	//assert(status == Suspended);
	status = saved_status;
	DBG_M_ACTIONS << "resumed: (status: " << status << ") " << *this << "\n";
	if (status == Suspended) status = Running;
}

void Action::recover() {
	DBG_M_ACTIONS << "Action failed to remove itself after completing: " << *this << "\n";
	assert(status == Complete || status == Failed);
	owner->stop(this);
}

void MachineInstance::clearAllActions() {
	while (!active_actions.empty()) {
		Action *a = active_actions.front();
 		active_actions.pop_front();
		
		DBG_M_ACTIONS << "Releasing action " << *a << "\n";
		// note that we can't delete an action with active triggers, instead we disable the trigger
		// and wait for the scheduler to delete the action
		if (a->getTrigger() && a->getTrigger()->enabled() && !a->getTrigger()->fired() )
			a->disableTrigger();
        a->release();
	}

	boost::mutex::scoped_lock(q_mutex);
	mail_queue.clear();
}

void MachineInstance::markActive() {
    is_active = true;
    active_machines.remove(this);
    active_machines.push_back(this);
}

void MachineInstance::markPassive() {
    is_active = false;
    active_machines.remove(this);
}

void MachineInstance::resume() {
	if (!is_enabled) {
		// adjust the starttime of the current state to make allowance for the 
		// time we were disabled
		struct timeval now;
		gettimeofday(&now, 0);
		long dt = get_diff_in_microsecs(&now, &disabled_time);
		start_time.tv_usec += dt % 1000000L;
		start_time.tv_sec += dt / 1000000L;
		if (start_time.tv_usec >= 1000000L) {
			start_time.tv_usec -= 1000000L;
			++start_time.tv_sec;
		}
		
		// fix status
		is_enabled = true; 
		error_state = 0;
        for (unsigned int i = 0; i<locals.size(); ++i) {
            locals[i].machine->resume();
        }
        setState(current_state, true);
	} 
	setNeedsCheck();
}

void MachineInstance::enable() {
    if (is_enabled) return;
    is_enabled = true; 
    error_state = 0; 
    clearAllActions(); 
    for (unsigned int i = 0; i<locals.size(); ++i) {
        if (!locals[i].machine) {
            std::stringstream ss;
            ss << "No machine found for " << locals[i].val << " in " << _name;
            char *msg = strdup(ss.str().c_str());
            MessageLog::instance()->add(msg);
            free(msg);
        }
        else
            locals[i].machine->enable();
    }
    if (_type == "LIST") // enabling a list enables the members
        for (unsigned int i = 0; i<parameters.size(); ++i) {
            if (parameters[i].machine) parameters[i].machine->enable();
            else {
                std::stringstream ss;
                ss << "No machine found for parameter " << parameters[i].val << " in " << _name;
                char *msg = strdup(ss.str().c_str());
                MessageLog::instance()->add(msg);
                free(msg);
            }
        }
    
    setInitialState();
	setNeedsCheck();
	// if any dependent machines are already enabled, make sure they know we are awake
	std::set<MachineInstance *>::iterator d_iter = depends.begin();
	while (d_iter != depends.end()) {
		MachineInstance *dep = *d_iter++;
		if (dep->enabled())dep->setNeedsCheck(); // make sure dependant machines update when a property changes
	}	
}

void MachineInstance::setDebug(bool which) {
	allow_debug = which;
	for (unsigned int i = 0; i<locals.size(); ++i) {
		locals[i].machine->setDebug(which);
	}
}

void MachineInstance::disable() { 
	is_enabled = false;
	if (locked) locked = 0;
    if (io_interface) {
        io_interface->turnOff();
    }
	gettimeofday(&disabled_time, 0); 
    if (_type == "LIST") // disabling a list disables the members
        for (unsigned int i = 0; i<parameters.size(); ++i) {
            if (parameters[i].machine) parameters[i].machine->disable();
        }
	for (unsigned int i = 0; i<locals.size(); ++i) {
		locals[i].machine->disable();
	}
	// if any dependent machines are already enabled, make sure they know we are disabled
	std::set<MachineInstance *>::iterator d_iter = depends.begin();
	while (d_iter != depends.end()) {
		MachineInstance *dep = *d_iter++;
		if (dep->enabled())dep->setNeedsCheck(); // make sure dependant machines update when a property changes
	}
}

void MachineInstance::resume(const std::string &state_name) { 
	if (!is_enabled) {		
		// fix status
		clearAllActions();
		is_enabled = true; 
		error_state = 0;
        // resume all local machines first so that setState() can
        // execute any necessary methods on local machines
        for (unsigned int i = 0; i<locals.size(); ++i) {
            locals[i].machine->resume();
        }
		setState(state_name.c_str(), true);
	}
    else if (state_name == "ALL") {
        // resume all local machines that have not already been enabled
        for (unsigned int i = 0; i<locals.size(); ++i) {
            if (!locals[i].machine->enabled())
                locals[i].machine->resume();
        }
    }
	setNeedsCheck();
}


void MachineInstance::push(Action *new_action) {
	if (!active_actions.empty()) {
		active_actions.back()->suspend();
		active_actions.back()->setBlocker(new_action);
	}
	DBG_M_ACTIONS << _name << " pushing " << *new_action << "\n";
	active_actions.push_back(new_action);
	return;
}


void MachineInstance::setStableState() {
	DBG_M_AUTOSTATES << _name << " checking stable states\n";
	if (!state_machine || !state_machine->allow_auto_states) {
        DBG_M_AUTOSTATES << _name << " aborting stable states check due to configuration\n";
        return;
    }
	if ( executingCommand() || !mail_queue.empty() ) {
        DBG_M_AUTOSTATES << _name << " aborting stable states check due to command execution\n";
		return;
	}
	
	// we must not set our stable state if objects we depend on are still updating their own state
	needs_check = 0;

    if (io_interface) {
	 	setState(io_interface->getStateString());
    }
    else if (_type == "LIST") {
        //DBG_MSG << _name << " has " << parameters.size() << " parameters\n";
        if (parameters.size())
            setState(State("nonempty"));
        else
            setState(State("empty"));
    }
    else if (_type == "REFERENCE") {
        //DBG_MSG << _name << " has " << parameters.size() << " parameters\n";
        if (locals.size())
            setState(State("ASSIGNED"));
        else
            setState(State("EMPTY"));
    }
    else {
		bool found_match = false;
        const long MAX_TIMER = 100000000L;
        long next_timer = MAX_TIMER; // during the search, we find the shortest timer value and schedule a wake-up
        for (unsigned int ss_idx = 0; ss_idx < stable_states.size(); ++ss_idx) {
            StableState &s = stable_states[ss_idx];
			// the following test should be enabled but there is currently a situation that 
			// can cause the stable state to not be evaluated when a trigger is set. TBD
			//		if ( (s.trigger && s.trigger->enabled() && s.trigger->fired() && s.condition(this) )
			//			|| (!s.trigger && s.condition(this)) ) {
			if (!found_match) {
                if (s.condition(this)) {
                    DBG_M_PREDICATES << _name << "." << s.state_name <<" condition " << *s.condition.predicate << " returned true\n";
                    if (current_state.getName() != s.state_name) {
                        if (s.subcondition_handlers) {
                            std::list<ConditionHandler>::iterator iter = s.subcondition_handlers->begin();
                            while (iter != s.subcondition_handlers->end()) {
                                ConditionHandler&ch = *iter++;
                                ch.triggered = false;
                            }
                        }
                        DBG_M_AUTOSTATES << _name << ":" << id << " (" << current_state << ") should be in state " << s.state_name 
                            << " due to condition: " << *s.condition.predicate << "\n";
                        MoveStateActionTemplate temp(_name.c_str(), s.state_name.c_str() );
                        state_change = new MoveStateAction(this, temp);
                        Action::Status action_status;
                        if ( (action_status = (*state_change)()) == Action::Failed) {
                            DBG_M_AUTOSTATES << " Warning: failed to start moving state on " << _name << " to " << s.state_name<< "\n";
                        }
                        else {
                            DBG_M_AUTOSTATES << " started state change on " << _name << " to " << s.state_name<<"\n";
                        }
                        //if (action_status == Action::Complete || action_status == Action::Failed) {
                        state_change->release();
                        state_change = 0;
                    }
                    else {
                        //DBG_M_AUTOSTATES << " already there\n";
                        // reschedule timer triggers for this state
                        if (s.uses_timer) {
                            //DBG_MSG << "Should retrigger timer for " << s.state_name << "\n";
                            Value v = getValue(s.timer_val.sValue);
                            if (v.kind == Value::t_integer && v.iValue < next_timer)
                                next_timer = v.iValue;
                        }
                    }
                    if (s.subcondition_handlers) {
                        std::list<ConditionHandler>::iterator iter = s.subcondition_handlers->begin();
                        while (iter != s.subcondition_handlers->end()) {
                            ConditionHandler *ch = &(*iter++);
                            ch->check(this);
                        }
                    }
                    found_match = true;
                    continue; // skip to the end of the stable state loop
                }
                else {
                    DBG_M_PREDICATES << _name << " " << s.state_name << " condition " << *s.condition.predicate << " returned false\n";
                    if (s.uses_timer) {
                        DBG_M_MESSAGING << _name << " scheduling condition tests for state " << s.state_name << "\n";
                        s.condition.predicate->scheduleTimerEvents(this);
                        //Value v = s.timer_val;
                        // there is a bug here; if the timer used in this state is on a different machine
                        // the schedule we generate has to be based on the timer of the other machine.
                        //if (v.kind == Value::t_integer && v.iValue < next_timer) {
                            // we would like to say  next_timer = v.iValue; here but since we have already been in this
                            // state for some time we need to say:
                        //    Value current_timer = *getTimerVal();
                        //    next_timer = v.iValue - current_timer.iValue;
                        //}
                    }
                }

	        }
            if (next_timer < MAX_TIMER) {
                /*
                 If only we could now just schedule a timer event:
                     Trigger *trigger = new Trigger("Timer");
                     Scheduler::instance()->add(new ScheduledItem(next_timer*1000, new FireTriggerAction(owner, trigger)));
                 unfortunately, the current code still has a bug because the 'uses_timer' flag of stable states
                 simply indicates that the state depends on *something* that uses a timer but that may be another
                 machine and that machine may not be in the state we are interested in.
                 
                 Workaround for now: check stable states every millisecond or so if any state uses a timer.
                 */
              /*  Trigger *trigger = new Trigger("Timer");
                Scheduler::instance()->add(new ScheduledItem(1000, new FireTriggerAction(this, trigger)));
                trigger->release(); */
            }
            // this state is not active so ensure its subcondition flags are turned off
            {
				if (s.subcondition_handlers) {
                    std::list<ConditionHandler>::iterator iter = s.subcondition_handlers->begin();
                    while (iter != s.subcondition_handlers->end()) {
                        ConditionHandler&ch = *iter++;
						if (ch.command_name == "FLAG" ) {
							MachineInstance *flag = lookup(ch.flag_name);
							if (flag) 
								flag->setState("off");
							else
								std::cerr << _name << " error: flag " << ch.flag_name << " not found\n"; 
						}
					}
				}
			}
	    }
	}
#if 0
	if (!needs_check) { BOOST_FOREACH(Transmitter *t, listens) {
		MachineInstance *m = dynamic_cast<MachineInstance *>(t);
		if (m && m!= this && m->state_machine && m->state_machine-> allow_auto_states && m->needs_check) {
			DBG_M_AUTOSTATES << _name << " waiting for " << m->getName() << " to stablize \n";
			//needs_check = true;
			return;
		}
	}}
#endif
}

void MachineInstance::setStateMachine(MachineClass *machine_class) {
    //if (state_machine) return;
    state_machine = machine_class;
	DBG_MSG << _name << " is of class " << machine_class->name << "\n";
	if (my_instance_type == MACHINE_INSTANCE && machine_class->allow_auto_states
            && (machine_class->stable_states.size() || machine_class->name == "LIST" || machine_class->name == "REFERENCE") )
		automatic_machines.push_back(this);
    BOOST_FOREACH(StableState &s, machine_class->stable_states) {
        stable_states.push_back(s);
        stable_states[stable_states.size()-1].setOwner(this); //
    }
    std::pair<Message, MachineCommandTemplate*>handler;
    BOOST_FOREACH(handler, machine_class->receives) {
        //DBG_M_INITIALISATION << "setting receive handler for " << handler.first << "\n";
        MachineCommand *mc = new MachineCommand(this, handler.second);
        mc->retain();
        receives_functions[handler.first] = mc;
    }
	// Warning: enter_functions are not used. TBD
    BOOST_FOREACH(handler, machine_class->enter_functions) {
        MachineCommand *mc = new MachineCommand(this, handler.second);
        mc->retain();
        enter_functions[handler.first] = mc;
    }
    std::pair<std::string, MachineCommandTemplate*> node;
    BOOST_FOREACH(node, state_machine->commands) {
        MachineCommand *mc = new MachineCommand(this, node.second);
        mc->retain();
        commands[node.first] = mc;
    }
	std::pair<std::string,Value> option;
	BOOST_FOREACH(option, machine_class->options) {
        DBG_INITIALISATION << _name << " initialising property " << option.first << " (" << option.second << ")\n";
		if (option.second.kind == Value::t_symbol) {
			Value v = getValue(option.second.sValue);
			if (v != SymbolTable::Null) 
				properties.add(option.first, v, SymbolTable::NO_REPLACE);
			else
				properties.add(option.first, option.second, SymbolTable::NO_REPLACE);
		}
		else
			properties.add(option.first, option.second, SymbolTable::NO_REPLACE);
	}
	properties.add("NAME", _name.c_str(), SymbolTable::ST_REPLACE);
    if (locals.size() == 0) {
        BOOST_FOREACH(Parameter p, state_machine->locals) {
            DBG_MSG << "cloning machine '" << p.val.sValue << "'\n";
            Parameter newp(p.val.sValue.c_str());
            if (!p.machine) {
                DBG_M_INITIALISATION << "failed to clone. no instance defined for parameter " << p.val.sValue << "\n";
            }
            else {
                newp.machine = new MachineInstance(p.val.sValue.c_str(), p.machine->_type.c_str());
                newp.machine->setProperties(p.machine->properties);
                newp.machine->setDefinitionLocation(p.machine->definition_file.c_str(), p.machine->definition_line);
                listenTo(newp.machine);
                newp.machine->addDependancy(this);
                std::map<std::string, MachineClass*>::iterator c_iter = machine_classes.find(newp.machine->_type);
                if (c_iter == machine_classes.end()) 
                    DBG_M_INITIALISATION <<"Warning: class " << newp.machine->_type << " not found\n"
                    ;
                else if ((*c_iter).second) {
                    newp.machine->owner = this;
                    MachineClass *newsm = (*c_iter).second;
                    newp.machine->setStateMachine(newsm);
                    if (newsm->parameters.size() != p.machine->parameters.size()) {
                        if (newsm->name == "LIST") {
                            DBG_MSG << "List has " << p.machine->parameters.size() << " parameters\n";
                            p.machine->setNeedsCheck();
                        }
                        if (newsm->name != "LIST" && (newsm->name != "POINT" || newsm->parameters.size() < 2 || newsm->parameters.size() >3 ) ) {
                            resetTemporaryStringStream();
                            ss << "## - Error: Machine " << newsm->name << " requires " 
                                << newsm->parameters.size()
                                << " parameters but instance " << _name << "." << newp.machine->getName() << " has " << p.machine->parameters.size();
                            error_messages.push_back(ss.str());
                            ++num_errors;
                        }
                    }
                    if (p.machine->parameters.size()) {
                        std::copy(p.machine->parameters.begin(), p.machine->parameters.end(), back_inserter(newp.machine->parameters));
                        DBG_M_INITIALISATION << "copied " << p.machine->parameters.size() << " parameters. local has " << p.machine->parameters.size() << "parameters\n";
                    }
                    if (p.machine->stable_states.size()) {
                        DBG_M_INITIALISATION << " restoring stable states for " << newp.val << "...before: " << newp.machine->stable_states.size();
                        newp.machine->stable_states.clear();
                        BOOST_FOREACH(StableState &s, p.machine->stable_states) {
                            newp.machine->stable_states.push_back(s);
                            newp.machine->stable_states[newp.machine->stable_states.size()-1].setOwner(this); //
                        }
                        std::copy(p.machine->stable_states.begin(), p.machine->stable_states.end(), back_inserter(newp.machine->stable_states));
                        DBG_M_INITIALISATION << " after: " << newp.machine->stable_states.size() << "\n";
                    }
                }
            }
           locals.push_back(newp);
        }
    }
    // a list has many parameters but the list class does not.
    // nevertheless, the parameters and dependencies still need to be setup.
    if (_type == "LIST") {
        for (unsigned int i=0; i<parameters.size(); ++i) {
            parameters[i].real_name = parameters[i].val.sValue;
            MachineInstance *m = lookup(parameters[i].real_name);
            if(m) {
                DBG_M_INITIALISATION << " found " << m->getName() << "\n";
                m->addDependancy(this);
                listenTo(m);
            }
            parameters[i].machine = m;
            setNeedsCheck();
            if (parameters.size())
                setState(State("nonempty"));
            else
                setState(State("empty"));
        }
    }
	// TBD check the parameter types
	size_t num_class_params = state_machine->parameters.size();
       for (unsigned int i=0; i<parameters.size(); ++i) {
           if (i<num_class_params) {
               if (parameters[i].val.kind == Value::t_symbol) {
                   parameters[i].real_name = parameters[i].val.sValue;
                   parameters[i].val = state_machine->parameters[i].val.sValue.c_str();
                   MachineInstance *m = lookup(parameters[i].real_name);
				DBG_M_INITIALISATION << _name << " is looking up " << parameters[i].real_name << " for param " << i << "\n";
				if(m) { 
					DBG_M_INITIALISATION << " found " << m->getName() << "\n"; 
					m->addDependancy(this);
					listenTo(m);
				}
                   parameters[i].machine = m;
               }
			else {
				DBG_M_MSG << "Parameter " << i << " (" << parameters[i].val << ") with real name " <<  state_machine->parameters[i].val << "\n";
			}
			// fix the properties for the machine passed as a parameter
			// the statemachine may nominate a default property that isn't 
			// provided by the actual parameter. here we find these situations
			// and set the default
			if (parameters[i].machine && !state_machine->parameters[i].properties.empty()) {
				SymbolTableConstIterator st_iter = state_machine->parameters[i].properties.begin();
				while(st_iter != state_machine->parameters[i].properties.end()) {
					std::pair<std::string, Value> prop = *st_iter++;
					if (!parameters[i].machine->properties.exists(prop.first.c_str())) {
						DBG_M_INITIALISATION << "copying default property " << prop.first << " on parameter " << i ;
						DBG_M_INITIALISATION << " to object" << *(parameters[i].machine) <<"\n";
						parameters[i].machine->properties.add(prop.first, prop.second, SymbolTable::NO_REPLACE);
					}
					else {
						DBG_M_INITIALISATION << "default property " << prop.first << " overridden by value " ;
						DBG_M_INITIALISATION << parameters[i].machine->properties.lookup(prop.first.c_str()) << "\n";
					}
				}
			}
		}
	}
    // copy transitions to support transtions on receipt of events from other machines
    BOOST_FOREACH(Transition &transition, state_machine->transitions) {
        transitions.push_back(transition);
        
        // copy this transition with the real name of the machine for the trigger
        std::string machine = transition.trigger.getText();
        if (machine.find('.') != std::string::npos) {
            machine.erase((machine.find('.')));
            // if this is a parameter, copy the transition to use the real name
            for (unsigned int i = 0; i < state_machine->parameters.size(); ++i) {
                if (parameters[i].val == machine) {
                    std::string evt = transition.trigger.getText();
                    evt = evt.substr(evt.find('.')+1);
                    std::string trigger = parameters[i].real_name + "." + evt;
                    transitions.push_back(Transition(transition.source, transition.dest,Message(trigger.c_str())));
                }
            }
        }
    }

	setupModbusInterface();
}

std::ostream &operator<<(std::ostream &out, const Action &a) {
    return a.operator<<(out);
}


Trigger *MachineInstance::setupTrigger(const std::string &machine_name, const std::string &message, const char *suffix = "_enter") {
	std::string trigger_name = machine_name;
	trigger_name += ".";
	trigger_name += message;
	trigger_name += suffix;
	DBG_M_MESSAGING << _name << " is waiting for message " << trigger_name << " from " << machine_name << "\n";
	return new Trigger(trigger_name);
}

const Value &MachineInstance::getValue(std::string property) {
	if (property.find('.') != std::string::npos) {
		// property is on another machine
		std::string name = property;
		name.erase(name.find('.'));
		std::string prop = property.substr(property.find('.')+1 );
		MachineInstance *other = lookup(name);
		if (other) {
			const Value &v = other->getValue(prop);
			DBG_M_PROPERTIES << other->getName() << " found property " << prop << " in machine " << name << " with value " << v << "\n";
			return v;
		}
        else if (_type == "REFERENCE" && name == "ITEM" && locals.size() == 0) {
            // permit references items to be not always available so that these can be
            // added and removed as the program executes
            return SymbolTable::Null;
        }
		else {
			resetTemporaryStringStream();
			ss << "could not find machine named " << name << " for property " << property;
			error_messages.push_back(ss.str());
			++num_errors;
			DBG_MSG << ss.str() << "\n";
			return SymbolTable::Null;
		}
	}
	else {
	    // try the current machine's parameters, the current instance of the machine, then the machine class and finally the global symbols
	
		// variables may refer to an initialisation value passed in as a parameter. 
		if (_type == "VARIABLE" || _type == "CONSTANT") {
			for (unsigned int i=0; i<parameters.size(); ++i) {
				if (state_machine->parameters[i].val.kind == Value::t_symbol 
						&& property == state_machine->parameters[i].val.sValue
						
					) {
					DBG_M_PREDICATES << _name << " found parameter " << i << " to resolve " << property << "\n";
					return parameters[i].val;
				}
			}
		}
		// use the global value if its name is mentioned in this machine's list of globals
        if (!state_machine) {
			resetTemporaryStringStream();
            ss << _name << " could not find a machine definition: " << _type;
            DBG_PROPERTIES << ss.str() << "\n";
            error_messages.push_back(ss.str());
            ++num_errors;
        }
		else if (state_machine->global_references.count(property)) {
			MachineInstance *m = state_machine->global_references[property];
			if (m) {
				DBG_M_PROPERTIES << _name << " using value property " << m->getValue("VALUE") << " from " << m->getName() << "\n";
				return m->getValue("VALUE");
			}
			else {
				resetTemporaryStringStream();
				ss << _name << " failed to find the machine for global: " << property;
				DBG_PROPERTIES << ss.str() << "\n";
				error_messages.push_back(ss.str());
				++num_errors;
			}
		}
	    DBG_M_PROPERTIES << getName() << " looking up property " << property << "\n";
		if (property == "TIMER") {
			// we do not use the precalculated timer here since this may be being accessed
			// within an action handler of a nother machine and will not have been updated
			// since the last evaluation of stable states.
            DBG_M_PROPERTIES << getName() << " timer: " << timer_val << "\n";
            getTimerVal();
			return timer_val; 
		}
		else if (SymbolTable::isKeyword(property.c_str())) {
			return SymbolTable::getKeyValue(property.c_str());
		}
	    else if (!properties.exists(property.c_str())) {
            if (state_machine) {
                if (!state_machine->properties.exists(property.c_str())) {
                    DBG_M_PROPERTIES << "no property " << property << " found in class, looking in globals\n";
                    if (globals.exists(property.c_str()))
                        return globals.lookup(property.c_str());
                }
                else {
                    DBG_M_PROPERTIES << "using property " << property << "from class\n";
                    return state_machine->properties.lookup(property.c_str()); 
                }
            }
	    }
		else {
			Value &x = properties.lookup(property.c_str());
			DBG_M_PROPERTIES << "found property " << property << " " << x.asString() << "\n";
			return x;
		}
		
		// finally, the 'property' may be a VARIABLE or CONSTANT machine declared locally
		MachineInstance *m = lookup(property);
		if (m) {
			if (m->_type == "VARIABLE" || m->_type == "CONSTANT") 
				return m->getValue("VALUE");
			else
				return *m->getCurrentStateVal();
		}
		
	}
	//is this a state?
	bool found = false;
    if (state_machine) {
        BOOST_FOREACH(State &s, state_machine->states) {
            if (s.getName() == property) {
                found = true;
                break;
            }
        }
    }
//	if (!found) {
//		std::stringstream ss;
//		ss << _name << " could not find property " << property;
//		error_messages.push_back(ss.str());
//		++num_errors;
//	}
	return SymbolTable::Null;
}

bool MachineInstance::hasState(const std::string &state_name) const {
	//is this a state?
    if (state_machine) {
        BOOST_FOREACH(State &s, state_machine->states) {
            if (s.getName() == state_name) {
                return true;
            }
        }
    }
    return false;
}

void MachineInstance::setValue(std::string property, Value new_value) {
	DBG_M_PROPERTIES << _name << " setvalue " << property << " to " << new_value << "\n";
	if (property.find('.') != std::string::npos) {
		// property is on another machine
		std::string name = property;
		name.erase(name.find('.'));
		std::string prop = property.substr(property.find('.')+1 );
		MachineInstance *other = lookup(name);
		if (other) {
			if (prop.length()) {
				DBG_M_PROPERTIES << *other << " setting property " << prop << " to " << new_value << "\n";
				other->setValue(prop, new_value);
			}
			else {
				DBG_MSG << _name << " bad request to set property " << property << "\n";
			}
		}
		else {
			DBG_PROPERTIES << "could not find machine named " << name << " for property " << property << "\n";
		}
	}
	else {
        // often variables are named in the GLOBAL list, if we find the property in that list
        // we try to change ask that machine to set its VALUE.
		if (state_machine->global_references.count(property)) {
			MachineInstance *global_machine = state_machine->global_references[property];
			if (global_machine && global_machine->_type != "CONSTANT")
                global_machine->setValue("VALUE", new_value);
			return;
		}
		if (property != "VALUE" && property != _name) {
			// the 'property' may be a VARIABLE or CONSTANT machine declared locally or globally
			MachineInstance *global_machine = lookup(property);
			if (global_machine) {
			 	if ( global_machine->_type != "CONSTANT" ) {
					global_machine->setValue("VALUE", new_value);
					return;
				}
				NB_MSG << "attempt to set a value on constant " << property << ". ignored\n";
				return;
			}
		}
		setNeedsCheck();
	    // try the current instance ofthe machine, then the machine class and finally the global symbols
	    DBG_M_PROPERTIES << getName() << " setting property " << property << " to " << new_value << "\n";
        Value &prev_value = properties.lookup(property.c_str());
        bool changed = (prev_value != new_value);
		{
            properties.add(property, new_value, SymbolTable::ST_REPLACE);
	        MessagingInterface *mif = MessagingInterface::getCurrent();
			resetTemporaryStringStream();
			if (owner) ss << owner->getName() << ".";
	        ss << _name << "." << property << " VALUE " << new_value << std::flush;
			if ( (property == "VALUE" || property == "value") && io_interface) {
				char buf[100];
				errno = 0;
				long value =0; // TBD deal with sign
				if (new_value.asInteger(value))
				{
					snprintf(buf, 100, "%s: updating output value to %ld\n", _name.c_str(),value); 
					io_interface->setValue( (uint32_t)value);
				}
				else {
					snprintf(buf, 100, "%s: could not set value to %s\n", _name.c_str(), new_value.asString().c_str());
				}
				MessageLog::instance()->add(buf);
			}
	        //mif->send(ss.str().c_str());
            mif->sendCommand("PROPERTY", fullName(), property.c_str(), new_value);

			if (getValue("PERSISTENT") == "true") {
				DBG_M_PROPERTIES << _name << " publishing change to persistent variable " << _name << "\n";
				//persistentStore->send(ss.str().c_str());
                persistentStore->sendCommand("PROPERTY", fullName(), property.c_str(), new_value);
                
			}
			// update modbus with the new value
			DBG_M_MODBUS << " building modbus name for property " << property << " on " << _name << "\n";
			std::string property_name;
			if (owner) {
				property_name = owner->getName();
				property_name += ".";
			}
			property_name += _name;
			if (property != "VALUE") {
				property_name += ".";
				property_name += property;
			}

			if (modbus_exports.count(property_name)){
				ModbusAddress ma = modbus_exports[property_name];
				if (property == "VALUE") {
					DBG_M_MODBUS << property_name << " modbus address " << ma << "\n";
				}
				switch(ma.getGroup()) {
					case ModbusAddress::none: 
						DBG_M_MODBUS << property_name << " export type is 'none'\n";
						break;
					case ModbusAddress::discrete:
					case ModbusAddress::coil:
					case ModbusAddress::input_register:
					case ModbusAddress::holding_register: {
						long intVal;
						if (ma.length() == 1 || ma.length() == 2) {
							if (new_value.asInteger(intVal)) {
								ma.update((int)intVal);
							}	
							else {
								DBG_M_MODBUS << property_name << " does not have an integer value\n";
							}
						}
						else if (new_value.kind == Value::t_string || new_value.kind == Value::t_symbol){
							ma.update(new_value.sValue);
						}
						else {
							DBG_M_MODBUS << "unable to export " << property_name << "\n";
						}
					}
				}
			}
			else
				DBG_M_MODBUS << _name << " " << property_name << " is not exported\n";
		}
        
        if (_type == "PUBLISHER" && mq_interface && property == "message" )
        {
            std::string old_val(properties.lookup(property.c_str()).asString());
            mq_interface->publish(properties.lookup("topic").asString(), old_val, this);
        }

        // only tell dependent machines to recheck predicates if the property
        // actually changes value
        if (changed) {
            DBG_M_PROPERTIES << "telling dependent machines about the change\n";
            std::set<MachineInstance *>::iterator d_iter = depends.begin();
            while (d_iter != depends.end()) {
                MachineInstance *dep = *d_iter++;
                dep->setNeedsCheck(); // make sure dependant machines update when a property changes
                //Message *msg = new Message("property_change");
                //send(msg, dep);
            }
        }
	}
}

void MachineInstance::refreshModbus(std::ostream &out) {

	std::map<int, std::string>::iterator iter = modbus_addresses.begin();
	while (iter != modbus_addresses.end()) {
		std::string full_name = (*iter).second;
        std::string short_name(full_name);
        if (short_name.rfind('.') != std::string::npos) {
            short_name = short_name.substr(short_name.rfind('.')+1);
        }
		int addr = (*iter).first & 0xffff;
		int group = (*iter).first >> 16;
		ModbusAddress info = modbus_exports[(*iter).second];
		if ((int)info.getGroup() != group) {
			NB_MSG << full_name << " index ("<<group<<"," <<addr<<")" << (*iter).first << " should be " << (*iter).second << "\n";
		}
		if ((int)info.getAddress() != addr) {
			NB_MSG << full_name << " index ("<<group<<"," <<addr<<")" << (*iter).first << " should be " << (*iter).second << "\n";
		}
		int length = info.length();
		std::string value("0");
		switch(info.getSource()) {
			case ModbusAddress::machine:
				if (_type == "VARIABLE" || _type=="CONSTANT") {
					value = properties.lookup("VALUE").asString();
				}
				else if (current_state.getName() == "on")
					value = "1";
				else 
					value = "0";
				break;
			case ModbusAddress::state:
				value =  (_name + "." + current_state.getName() == (*iter).second) ? "1" : "0";
				break;
			case ModbusAddress::command:
				value = "0";
				break;
			case ModbusAddress::property:
				if (properties.exists( short_name.c_str() ))
					value = properties.lookup( short_name.c_str() ).asString();
				else {
					MachineInstance *mi = lookup( (*iter).second );
					if (mi && (mi->_type == "VARIABLE" || mi->_type == "CONSTANT") ) {
						const Value &x = mi->getValue("VALUE");
						value = x.asString();
					}
					else {
						NB_MSG << "Error: " << full_name <<" has not been initialised\n";
					}
				}
				break;
			case ModbusAddress::unknown:
				value = "UNKNOWN";
		}
		if (owner)
			out << group << " " << addr << " " << owner->getName() << "." << full_name << " " << length << " " << value << "\n";
		else
			out << group << " " << addr << " " << full_name << " " << length << " " << value << "\n";
		
		iter++;
	}
}
void MachineInstance::exportModbusMapping(std::ostream &out) {
	
	std::map<int, std::string>::iterator iter = modbus_addresses.begin();
	while (iter != modbus_addresses.end()) {
		int addr = (*iter).first & 0xffff;
		int group = (*iter).first >> 16;
		ModbusAddress info = modbus_exports[(*iter).second];
		//assert((int)info.getGroup() == group);
		//assert(info.getAddress() == addr);
		const char *data_type;
		switch(ModbusAddress::toGroup(group)) {
			case ModbusAddress::discrete: data_type = "Discrete"; break;
			case ModbusAddress::coil: data_type = "Discrete"; break;
			case ModbusAddress::input_register: 
			case ModbusAddress::holding_register:  
				if (info.length() == 1)
					data_type = "Signed_int_16";
				else if (info.length() == 2)
					data_type = "Signed_int_32";
				else
					data_type = "Ascii_String";
				break;
			default: data_type = "Unknown";
		}
		if (owner)
			out << group << ":" << std::setfill('0') << std::setw(5) << addr
			<< "\t" << owner->getName() << "." << (*iter).second
			<< "\t" << data_type
			<< "\n";
		else
			out << group << ":" << std::setfill('0') << std::setw(5) << addr 
			<< "\t" << (*iter).second 
			<< "\t" << data_type
			<< "\n";
		iter++;
	}
	
}

// Modbus


ModbusAddress MachineInstance::addModbusExport(std::string name, ModbusAddress::Group g, unsigned int n, 
		ModbusAddressable *owner, ModbusAddress::Source src, const std::string &full_name) {
	if (ModbusAddress::preset_modbus_mapping.count(full_name) != 0) {
		ModbusAddressDetails info = ModbusAddress::preset_modbus_mapping[full_name];
		DBG_MODBUS << _name << " " << name << " has predefined address: " << info.group<<":"<<std::setfill('0')<<std::setw(5)<<info.address << "\n";
		ModbusAddress addr(ModbusAddress::toGroup(info.group), info.address, info.len, owner, src, full_name);
		modbus_exports[name] = addr;
		return addr;
	}
	else {
		ModbusAddress addr = ModbusAddress::alloc(g, n, this, src, full_name);
		DBG_MODBUS << _name << " " << name << " allocated new address " << addr << "\n";
		modbus_exports[name] = addr;
		return addr;
	}
}

void MachineInstance::setupModbusPropertyExports(std::string property_name, ModbusAddress::Group grp, int size) {
	std::string full_name;
	if (owner) full_name = owner->getName() + ".";
	std::string name;
	if (_name != property_name) name = _name + ".";
	name += property_name;
	switch(grp) {
		case ModbusAddress::discrete:
			addModbusExport(name, ModbusAddress::discrete, size, this, ModbusAddress::property, full_name+name); 
		break;
		case ModbusAddress::coil:
			addModbusExport(name, ModbusAddress::coil, size, this, ModbusAddress::property, full_name+name); 
		break;
		case ModbusAddress::input_register:
			addModbusExport(name, ModbusAddress::input_register, size, this, ModbusAddress::property, full_name+name); 
		break;
		case ModbusAddress::holding_register:
			addModbusExport(name, ModbusAddress::holding_register, size, this, ModbusAddress::property, full_name+name); 
		break;
		default: ;
	}
}

void MachineInstance::setupModbusInterface() {

	if (modbus_addresses.size() != 0) return; // already done
	DBG_MODBUS << fullName() << " setting up modbus\n";
	std::string full_name = fullName();

	bool self_discrete = false;
	bool self_coil = false;
	bool self_reg = false;
	bool self_rwreg = false;
	
	
	// workout the export type for this machine
	ExportType export_type = none;
		
	bool exported = properties.exists("export");
	if (exported) {
		Value export_type_val = properties.lookup("export");
		if (export_type_val != "false") {
			if (_type == "POINT") {
				Value &type = properties.lookup("type");
				if (type != SymbolTable::Null) {
					if (type == "Input") {
						self_discrete = true; 
						export_type=discrete;
					}
					else if (type == "Output") {
						if (export_type_val=="rw") {
							self_coil = true;
							export_type=coil;
						}
						else {
							self_discrete = true;
							export_type=coil;
						}
					}
				}
			}
			else
				if (export_type_val=="rw") {
					self_coil = true;
					export_type=coil;
				}
				else if (export_type_val=="reg") {
					self_reg = true;
					export_type=reg;
				}
				else if (export_type_val=="rw_reg") {
					self_rwreg = true;
					export_type=rw_reg;
				}
				else if (export_type_val=="reg32") {
					self_reg = true;
					export_type=reg32;
				}
				else if (export_type_val=="rw_reg32") {
					self_rwreg = true;
					export_type=rw_reg32;
				}
				else if (export_type_val=="str") {
					self_reg = true; 
					export_type=str;
				}
				else {
					self_discrete = true;
					export_type=discrete;
				}
		}
	}
	if (_type != "VARIABLE" && _type != "CONSTANT") {// export property refers to VALUE export type, not the machine
		modbus_exported = export_type;		
	}

	// register the individual properties 
	
	if (_type != "VARIABLE" && _type != "CONSTANT") {
		// exporting 'self' (either 'on' state or 'VALUE' property depending on export type)
		if (self_coil) {
			modbus_address = addModbusExport(_name, ModbusAddress::coil, 1, this, ModbusAddress::machine, full_name);
		}
		else if (self_discrete) {
			modbus_address = addModbusExport(_name, ModbusAddress::discrete, 1, this, ModbusAddress::machine, full_name);
			//addModbusExportname(_name].setName(full_name);
		}
		else if (self_reg) {
			int len = 1;
			if (export_type == reg) {
				len = 1;
			}
			else if (export_type == reg32) {
				len = 2;
			}
			else if (export_type == str) {
				len=80;
			}
			modbus_address = addModbusExport(_name, ModbusAddress::input_register, len, this, ModbusAddress::machine, full_name);
			//modbus_exports[_name].setName(full_name);
		}
		else if (self_rwreg) {
			int len = 1;
			if (export_type == rw_reg32) {
				len = 2;
			}
			addModbusExport(_name, ModbusAddress::holding_register, len, this, ModbusAddress::machine, full_name);
			//modbus_exports[name(_name].setName(full_name);
		}
	}

	// exported states
	BOOST_FOREACH(std::string s, state_machine->state_exports) {
		std::string name = _name + "." + s;
		addModbusExport(name, ModbusAddress::discrete, 1, this, ModbusAddress::state, full_name+"."+s);
		//modbus_exports[name(name].setName(full_name+"."+s);
	}

	// exported commands
	BOOST_FOREACH(std::string s, state_machine->command_exports) {
		std::string name = _name + "." + s;
		addModbusExport(name, ModbusAddress::coil, 1, this, ModbusAddress::command, full_name+"."+s);
		//addModbusExportname(name].setName(full_name+"."+s);
	}
	
	if (_type == "VARIABLE" || _type == "CONSTANT") {
		std::string property_name(_name);
		switch(export_type) {
			case none: break;
			case discrete:
				setupModbusPropertyExports(property_name, ModbusAddress::discrete, 1);
				break;
			case coil:
				setupModbusPropertyExports(property_name, ModbusAddress::coil, 1);
				break;
			case reg:
				setupModbusPropertyExports(property_name, ModbusAddress::input_register, 1);
				break;
			case rw_reg:
				setupModbusPropertyExports(property_name, ModbusAddress::holding_register, 1);
				break;
			case reg32:
				setupModbusPropertyExports(property_name, ModbusAddress::input_register, 2);
				break;
			case rw_reg32:
				setupModbusPropertyExports(property_name, ModbusAddress::holding_register, 2);
				break;
			case str:
				DBG_M_MSG <<" Warning: String property export not supported\n";
		}
	}

	BOOST_FOREACH(ModbusAddressTemplate mat, state_machine->exports) {
		//if (export_type == none) continue;
		setupModbusPropertyExports(mat.property_name, mat.kind, mat.size);
	}

	assert(modbus_addresses.size() == 0);
	std::map<std::string, ModbusAddress >::iterator iter = modbus_exports.begin();
	while (iter != modbus_exports.end()) {
		const ModbusAddress &ma = (*iter).second;
		int index = ((int)ma.getGroup() << 16) + ma.getAddress();
		if ( modbus_addresses.count(index) != 0) {
			NB_MSG << _name << " " << (*iter).first << " address " << ma << " already recorded as " << modbus_addresses[index] << "\n";
		}
		assert(modbus_addresses.count(index) == 0);
		modbus_addresses[index] = (*iter).first;
		iter++;
	}
	if (modbus_addresses.size() != modbus_exports.size()){
		NB_MSG << _name << " modbus export inconsistent. addresses: " << modbus_addresses.size() << " indexes " << modbus_exports.size() << "\n";
	}
	assert(modbus_addresses.size() == modbus_exports.size());
	
}


void MachineInstance::modbusUpdated(ModbusAddress &base_addr, unsigned int offset, int new_value) {
	std::string name = fullName();
	DBG_M_MODBUS << name << " modbusUpdated " << base_addr << " " << offset << " " << new_value << "\n";
	int index = (base_addr.getGroup() <<16) + base_addr.getAddress() + offset;
	if (!modbus_addresses.count(index)) {
		NB_MSG << name << " Error: bad modbus address lookup for " << base_addr << "\n";
		return;
	}
	std::string item_name = modbus_addresses[index];
	if (!modbus_exports.count(item_name)) {
		NB_MSG << name << " Error: bad modbus name lookup for " << item_name << "\n";
		return;
	}
	ModbusAddress addr = modbus_exports[item_name];
	DBG_M_MODBUS << name << " local ModbusAddress found: " << addr<< "\n";	

	if (addr.getGroup() == ModbusAddress::coil || addr.getGroup() == ModbusAddress::discrete){
		
		if (addr.getSource() == ModbusAddress::machine) {
			// crosscheck - the machine must have been exported read/write
			assert(properties.exists("export"));
			//Value &export_type = properties.lookup("export");
            // disabling this test because does not seem valid when talking with a PLC instead of a panel
			//assert( (export_type.kind == Value::t_string || export_type.kind == Value::t_symbol)
			//	&& export_type.sValue == "rw");
			//assert(addr.getOffset() == 0);

			if (new_value) {
				SetStateActionTemplate ssat(CStringHolder("SELF"), State("on") );
				active_actions.push_front(ssat.factory(this)); // execute this state change once all other actions are complete
			}
			else {
				SetStateActionTemplate ssat(CStringHolder("SELF"), State("off") );
				active_actions.push_front(ssat.factory(this)); // execute this state change once all other actions are complete
			}
			return;
		}
		else if (addr.getSource() == ModbusAddress::command) {
			if (new_value) {
				std::string &cmd_name = modbus_addresses[index];
				DBG_M_MODBUS << _name << " executing command " << cmd_name << "\n";
				// execute this command once all other actions are complete
				handle(Message(cmd_name.c_str()), this, true); // fire the trigger when the command is done
			}
			return;
		}
		else if (addr.getSource() == ModbusAddress::property) {
			std::string property_name = modbus_addresses[index];
			DBG_M_MODBUS << _name << " set property " << property_name << " via modbus index " << index << " (" << addr << ")\n";
            if (name == _name)
                setValue("VALUE", new_value);
            else
                setValue(property_name, new_value);
		}
		else
			DBG_M_MODBUS << _name << " received update for out-of-range coil" << offset << "\n";
		
	}
	else if (addr.getGroup() == ModbusAddress::holding_register) {
		DBG_M_MODBUS << name << " holding register update\n";
		std::string property_name = modbus_addresses[index];
		DBG_M_MODBUS << _name << " set property " << property_name << " via modbus index " << index << " (" << addr << ")\n";
        if (property_name == _name)
            setValue("VALUE", new_value);
        else
            setValue(property_name, new_value);
	}
	else {
		NB_MSG << name << " unexpected modbus group for write operation " << addr << "\n";
	}
	//++needs_check;
}

int MachineInstance::getModbusValue(ModbusAddress &addr, unsigned int offset, int len) {
	int pos = (addr.getGroup() << 16) + addr.getAddress() + offset;
	if (!modbus_addresses.count(pos)) {
		DBG_M_MODBUS << _name << " unknown modbus address offset requested " << offset << "\n";
		return 0;
	}
	// self
	std::string devname = modbus_addresses[pos];
	DBG_M_MODBUS << _name << " found device " << devname << " matching address " << addr.getGroup() << ":" << addr.getAddress()  + offset << "\n";
	if (devname == _name && addr.getAddress() == 0) {
		if (_type != "VARIABLE" && _type != "CONSTANT") {
			if (current_state.getName() == "on")
				return 1;
			else
				return 0;
		}
		else {
			const Value &v = getValue("VALUE");
			long intVal;
			if (v.asInteger(intVal))
				return (int)intVal;
			else
				return 0;
		}
	}
	return 0;
}



// Error states (not used yet)
bool MachineInstance::inError() { 
	return error_state != 0; 
}

void MachineInstance::setError(int val) { 
	if (error_state) return;
	error_state = val; 
	if (val) { 
		saved_state = current_state; 
		setState("ERROR");
	} 
}
void MachineInstance::resetError() {
	error_state = 0; 
	if (state_machine) setState(state_machine->initial_state); 
}
void MachineInstance::ignoreError() {
	error_state = 0;
}
