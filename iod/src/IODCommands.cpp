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

#include <stdlib.h>
#include "IODCommands.h"
#include "MachineInstance.h"
#include "IOComponent.h"
#include <list>
#include "Logger.h"
#include "DebugExtra.h"
#include "cJSON.h"
#include <fstream>
#include "options.h"
#include "Statistic.h"
#include "Statistics.h"
#include "MessageLog.h"
#include "MessageEncoding.h"
#include "Channel.h"
#include "MessagingInterface.h"
#include "Scheduler.h"
#ifndef EC_SIMULATOR
#include "ECInterface.h"
#endif

extern Statistics *statistics;

std::map<std::string, std::string>message_handlers;

SequenceNumber IODCommand::sequences;

extern bool program_done;

bool IODCommandGetStatus::run(std::vector<Value> &params) {
	bool ok = false;
	if (params.size() == 2) {

		std::string ds = params[1].asString();
		IOComponent *device = IOComponent::lookup_device(ds);
		if (device) {
			ok = true;
			std::string res = device->getStateString();
			if (device->address.bitlen>1) {
				char buf[10];
				snprintf(buf, 9, "(%d)", device->value());
				res += buf;
			}
			result_str = res;
		}
		else {
			MachineInstance *machine = MachineInstance::find(params[1].asString().c_str());
			if (machine) {
				ok = true;
				result_str = machine->getCurrentStateString();
			}
			else
				error_str = "Not Found";
		}
	}
	return ok;
}

bool IODCommandSetStatus::run(std::vector<Value> &params) {
	std::string ds;
	std::string state_name;
    if (params.size() == 4) { // SET machine TO state
        ds = params[1].asString();
		state_name = params[3].asString();
	}
	else if (params.size() == 3) { // STATE machine state
		ds = params[1].asString();
		state_name = params[2].asString();
	}
	else {
		error_str = "Usage: SET device TO state";
		return false;
	}

	Output *device = dynamic_cast<Output *>(IOComponent::lookup_device(ds));
	if (device) {
		if (state_name == "on")
			device->turnOn();
		else if (state_name == "off")
			device->turnOff();
		result_str = device->getStateString();
		return true;
	}
	else {
		MachineInstance *mi = MachineInstance::find(ds.c_str());
		if (mi) {
			/* it would be safer to push the requested state change onto the machine's
				action list but some machines do not poll their action list because they
				do not expect to receive events
			*/
			if (mi->transitions.size()) {
			   SetStateActionTemplate ssat(CStringHolder(strdup(ds.c_str())), state_name );
			   mi->enqueueAction(ssat.factory(mi)); // execute this state change once all other actions are complete
			}
			else {
				;
				if (!mi->getStateMachine()) {
					char buf[150];
					snprintf(buf, 150, "Error: machine %s has no state machine", mi->getName().c_str());
					MessageLog::instance()->add(buf);
					error_str = buf;
					return false;
				}
				State *s = mi->getStateMachine()->findState(state_name.c_str());
				if (!s) {
					if (mi->isShadow()) {
						s = &mi->getStateMachine()->initial_state;
					}
					else {
						char buf[150];
						snprintf(buf, 150, "Error: machine %s has no state called '%s'", mi->getName().c_str(), state_name.c_str());
						MessageLog::instance()->add(buf);
						error_str = buf;
						return false;
					}
				}
				mi->setState(*s);
			}
			result_str = "OK";
			return true;
		}
	}
	//  Send reply back to client
	const char *msg_text = "Not found: ";
	size_t len = strlen(msg_text) + ds.length();
	char *text = (char *)malloc(len+1);
	sprintf(text, "%s%s", msg_text, ds.c_str());
	error_str = text;
	free(text);
	return false;
}


bool IODCommandEnable::run(std::vector<Value> &params) {
	std::cout << "received iod command ENABLE " << params[1] << "\n";
	if (params.size() == 2) {
		DBG_MSG << "enabling " << params[1] << "\n";
		MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
		if (m && !m->enabled()) m->enable();
		result_str = "OK";
		return true;
	}
	error_str = "Failed to find machine";
	return false;
}

bool IODCommandResume::run(std::vector<Value> &params) {
	std::cout << "received iod command RESUME " << params[1] << "\n";
	if (params.size() == 2) {
		DBG_MSG << "resuming " << params[1] << "\n";
		MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
		if (m && !m->enabled()) m->resume();
		result_str = "OK";
		return true;
	}
	else if (params.size() == 4 && params[2] == "AT") {
		DBG_MSG << "resuming " << params[1] << " at state "  << params[3] << "\n";
		MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
		if (m && !m->enabled()) m->resume();
		result_str = "OK";
		return true;
	}
	error_str = "Failed to find machine";
	return false;
}

    bool IODCommandDisable::run(std::vector<Value> &params) {
		std::cout << "received iod command DISABLE " << params[1] << "\n";
        if (params.size() == 2) {
			DBG_MSG << "disabling " << params[1] << "\n";
			MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
			if (m && m->enabled()) m->disable();
			result_str = "OK";
			return true;
		}
		error_str = "Failed to find machine";
		return false;
	}


    bool IODCommandDescribe::run(std::vector<Value> &params) {
        std::cout << "received iod command Describe " << params[1] << "\n";
        bool use_json = false;
        if (params.size() == 3 && params[2] != "JSON") {
            error_str = "Usage: DESCRIBE machine [JSON]";
            return false;
        }
        if (params.size() == 2 || params.size() == 3) {
            MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
            cJSON *root;
            if (use_json)
                root = cJSON_CreateArray();
            std::stringstream ss;
            if (m)
                    m->describe(ss);
            else
                ss << "Failed to describe unknown machine " << params[1];
            if (use_json) {
                std::istringstream iss(ss.str());
                char buf[500];
                while (iss.getline(buf, 500, '\n')) {
                    cJSON_AddItemToArray(root, cJSON_CreateString(buf));                
                }
                char *res = cJSON_Print(root);
                cJSON_Delete(root);
                result_str = res;
                delete res;
            }
            else
                result_str = ss.str();
            return true;
        }
        error_str = "Failed to find machine";
        return false;
    }


    bool IODCommandToggle::run(std::vector<Value> &params) {
        if (params.size() == 2) {
			DBG_MSG << "toggling " << params[1] << "\n";
			size_t pos = params[1].asString().find('-');
			std::string machine_name = params[1].asString();
			if (pos != std::string::npos) machine_name.erase(pos);
            MachineInstance *m = MachineInstance::find(machine_name.c_str());
		    if (m) {
				if (pos != std::string::npos) {
					machine_name = params[1].asString().substr(pos+1);
					m = m->lookup(machine_name);
					if (!m) {
						error_str = "No such machine";
						return false;
					}
				}
				if (!m->enabled()) {
					error_str = "Device is disabled";
					return false;
				}
		  	 	if (m->_type != "POINT") {
				     Message *msg;
                    if (m->getCurrent().is(ClockworkToken::on)) {
                        if (m->receives(Message("turnOff"), 0)) {
                            msg = new Message("turnOff");
                            m->sendMessageToReceiver(msg, m);
                        }
                        else {
                            State *s = m->getStateMachine()->findState("off");
                            if (s) m->setState(*s);
                        }
                    }
                    else if (m->getCurrent().is(ClockworkToken::off) ) {
                        if (m->receives(Message("turnOn"), 0)) {
                            msg = new Message("turnOn");
                            m->sendMessageToReceiver(msg, m);
                        }
                        else{
                            State *s = m->getStateMachine()->findState("on");
                            if (s) m->setState(*s);
                        }
                    }
                    result_str = "OK";
                    return true;
				}
			}
			else {
				error_str = "Usage: toggle device_name";
				return false;
		    }
				
            Output *device = dynamic_cast<Output *>(IOComponent::lookup_device(params[1].asString()));
            if (device) {
                if (device->isOn()) device->turnOff();
                else if (device->isOff()) device->turnOn();
				else {
					error_str = "device is neither on nor off\n";
					return false;
				}
                result_str = "OK";
                return true;
            }
            else {
                // MQTT
                if (m->getCurrent().getName() == "on" || m->getCurrent().getName() == "off") {
                    std::string msg_str = m->getCurrent().getName();
                    if (msg_str == "off")
                        msg_str = "on_enter";
                    else
                        msg_str = "off_enter";
                    //m->mq_interface->send(new Message(msg_str.c_str()), m);
                    m->execute(new Message(msg_str.c_str()), m->mq_interface);
                    result_str = "OK";
                    return true;
                }
                else {
                    error_str = "Unknown message for a POINT";
                    return false;
                }
            }
        }
		else {
			error_str = "Unknown device";
			return false;
		}
	}

    bool IODCommandGetProperty::run(std::vector<Value> &params) {
	    MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
	    if (m) {
			if (params.size() != 3) {
				error_str = "Error: usage is GET machine property";
				return false;
			}
			Value &v = m->getValue(params[2].asString());
			if (v == SymbolTable::Null) {
				error_str = "Error: property not found";
				return false;
			}
			else {
				result_str = v.asString();
				return true;
			}
		}
		else {
			error_str = "Error: Unknown device";
			return false;
		}
		
	}

    bool IODCommandProperty::run(std::vector<Value> &params) {
        //if (params.size() == 4) {
		    MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
		    if (m) {
                if (params.size() != 4)
				{
                    error_str = "Usage: PROPERTY machine property value";
                    return false;
				}
                else if (params.size() == 4) {
                    if (m->debug()) {
                        DBG_MSG << "setting property " << params[1] << "." << params[2] << " to " << params[3] << "\n";
                    }
					if (params[3].kind == Value::t_string || params[3].kind == Value::t_symbol) {
	                    long x;
	                    char *p;
	                    x = strtol(params[3].asString().c_str(), &p, 0);
	                    if (*p == 0)
	                        m->setValue(params[2].asString(), x);
	                    else
	                        m->setValue(params[2].asString(), params[3]);
					}
					else {
	                    m->setValue(params[2].asString(), params[3]);
					}
                }
                else {
                    // extra parameters implies the value contains spaces so 
					// we find the tail of the parameter string and use that for the property value
                    size_t pos = raw_message_.find(params[2].asString().c_str());
                    if (pos == std::string::npos) {
                        error_str = "Unexpected parameter error ";
                        return false;
                    }
                    pos += params[2].asString().length();
                    while (raw_message_[pos] == ' ') ++pos; // skip the parameter spacing
                    const char *p = raw_message_.c_str() + pos;
                    m->setValue(params[2].asString(), p);
                }

                result_str = "OK";
                return true;
			}
	        else {
				char buf[200];
				snprintf(buf, 200, "Unknown device: %s", params[1].asString().c_str() );
	            error_str = buf;
	            return false;
	        }
		/*}
         else {
			std::stringstream ss;
			ss << "Unrecognised parameters in ";
			std::ostream_iterator<std::string> out(ss, " ");
			std::copy(params.begin(), params.end(), out);
			ss << ".  Usage: PROPERTY property_name value";
			error_str = ss.str();
			return false;
		}
         */
	}

    bool IODCommandList::run(std::vector<Value> &params) {
        std::ostringstream ss;
        std::map<std::string, MachineInstance*>::const_iterator iter = machines.begin();
        while (iter != machines.end()) {
            MachineInstance *m = (*iter).second;
            ss << (m->getName()) << " " << m->_type;
            if (m->_type == "POINT") ss << " " << m->properties.lookup("tab");
            ss << "\n";
            iter++;
        }
        result_str = ss.str();
        return true;
    }

std::set<std::string> IODCommandListJSON::no_display;

cJSON *printMachineInstanceToJSON(MachineInstance *m, std::string prefix = "") {
    cJSON *node = cJSON_CreateObject();
    std::string name_str = m->getName();
    if (prefix.length()!=0) {
        std::stringstream ss;
        ss << prefix << '-' << m->getName()<< std::flush;
        name_str = ss.str();
    }
    cJSON_AddStringToObject(node, "name", name_str.c_str());
    cJSON_AddStringToObject(node, "class", m->_type.c_str());
 #ifndef EC_SIMULATOR
    if (m->io_interface) {
		IOComponent *ioc = m->io_interface;
		cJSON_AddNumberToObject(node, "module", ioc->address.module_position);
		ECModule *mod = ECInterface::findModule(ioc->address.module_position);
		if (mod) {
			cJSON_AddStringToObject(node, "module_name", mod->name.c_str());
		}
	}
 #endif
 
    SymbolTableConstIterator st_iter = m->properties.begin();
    while (st_iter != m->properties.end()) {
	    std::pair<std::string, Value> item(*st_iter++);
	    if (item.second.kind == Value::t_integer)
	        cJSON_AddNumberToObject(node, item.first.c_str(), item.second.iValue);
	    else
	        cJSON_AddStringToObject(node, item.first.c_str(), item.second.asString().c_str());
    }
	Action *action;
	if ( (action = m->executingCommand()))  {
		std::stringstream ss;
		ss << *action;
        cJSON_AddStringToObject(node, "executing", ss.str().c_str());
	}

	if (m->enabled()) 
		cJSON_AddTrueToObject(node, "enabled");
	else 
		cJSON_AddFalseToObject(node, "enabled");
    if (!m->io_interface) {
        size_t len = m->getCurrent().getName().length()+1;
		char *cs = (char*)malloc(len);
        memcpy(cs, m->getCurrentStateString(), len);
        cJSON_AddStringToObject(node, "state", cs);
        free(cs);
    }
    else {
           IOComponent *device = IOComponent::lookup_device(m->getName().c_str());
           if (device) {
               const char *state = device->getStateString();
               cJSON_AddStringToObject(node, "state", state);
           }
    }
    if (m->commands.size()) {
		std::stringstream cmds;
		std::pair<std::string, MachineCommand*> cmd;
		const char *delim = "";
		BOOST_FOREACH(cmd, m->commands) {
			cmds << delim << cmd.first;
			delim = ",";
		}
		cmds << std::flush;
		cJSON_AddStringToObject(node, "commands", cmds.str().c_str());
    }
/*
    if (m->receives_functions.size()) {
		std::stringstream cmds;
		std::pair<Message, MachineCommand*> rcv;
		const char *delim = "";
		BOOST_FOREACH(rcv, m->receives_functions) {
			cmds << delim << rcv.first;
			delim = ",";
		}
		cmds << std::flush;
		cJSON_AddStringToObject(node, "receives", cmds.str().c_str());
    }
*/
	if (m->properties.size()) {
		std::stringstream props;
		SymbolTableConstIterator st_iter = m->properties.begin();
		int count = 0;
		const char *delim="";
		while (st_iter != m->properties.end()) {
			std::pair<std::string, Value> prop = *st_iter++;
			if (IODCommandListJSON::no_display.count(prop.first)) continue;
			props << delim << prop.first;
			delim = ",";
			++count;
		}
		if ( (action = m->executingCommand()))  {
			props << delim << "executing";
		}
		props << std::flush;
		if (count) cJSON_AddStringToObject(node, "display", props.str().c_str());
	}
    return node;
}


    bool IODCommandListJSON::run(std::vector<Value> &params) {
        cJSON *root = cJSON_CreateArray();
		Value tab("");
		bool limited = false;
		if (params.size() > 2) {
			tab = params[2];
			limited = true;
		}
        std::map<std::string, MachineInstance*>::const_iterator iter = machines.begin();
        while (iter != machines.end()) {
            MachineInstance *m = (*iter++).second;
			if (!limited || (limited && m->properties.lookup("tab") == tab) ) {
   		        cJSON_AddItemToArray(root, printMachineInstanceToJSON(m));
                for (unsigned int idx = 0; idx < m->locals.size(); ++idx) {
                    const Parameter &p = m->locals[idx];
    	       		cJSON_AddItemToArray(root, printMachineInstanceToJSON(p.machine, m->getName()));
	        	}
			}
        }
        char *res = cJSON_Print(root);
        cJSON_Delete(root);
        bool done;
		char *p = res; while (*p) { *p &= 0x7f; ++p; }
        if (res) {
            result_str = res;
            free(res);
            done = true;
        }
        else {
            error_str = "JSON error";
            done = false;
        }
        return done;
    }


/*
	send a message. The message may be in one of the forms: 
		machine-object.command or
		object.command
*/
    bool IODCommandSend::run(std::vector<Value> &params) {

		MachineInstance *m = 0;
        std::string machine_name;
        std::string command;

        // the ROUTE command may have been used to reroute this message:
        if (params.size() == 2) {
            command = params[1].asString();
            if (message_handlers.count(command)) {
                const std::string &name = message_handlers[command];
                m = MachineInstance::find(name.c_str());
                if (m) machine_name = m->fullName();
            }
        }
		
        // we may have found the target machine already in the
        // message routing table. if not, continue searching
        if (!m) {
            if ( (params.size() != 2 && params.size() != 4)
                    || (params.size() == 4 && params[2] != "TO" && params[2] != "to")
                ) {
                error_str = "Usage: SEND machine.command | SEND command TO machine ";
                return false;
            }
            
            // if the SEND machine.command syntax was used, we keep everything after the last '.'
            // if SEND command TO machine was used, we don't touch the command string
            if (params.size() == 2) {
                machine_name =  params[1].asString();
                command = params[1].asString();

                size_t sep = command.rfind('.');
                if (sep != std::string::npos) {
                    command.erase(0,sep+1);
                    machine_name.erase(sep);
                }
            }
            else if (params.size() == 4) {
                command = params[1].asString();
                machine_name = params[3].asString();
            }
            size_t pos = machine_name.find('-');
            if (pos != std::string::npos) {
                std::string submachine = machine_name.substr(pos+1);
                machine_name.erase(pos);
                pos = submachine.find('.');
                if (pos != std::string::npos)
                    submachine.erase(pos);
                MachineInstance *owner = MachineInstance::find(machine_name.c_str());
                if (!owner) {
                    error_str = "could not find machine";
                    return false;
                }
                m = owner->lookup(submachine); // find the actual device
            }
            else
                m = MachineInstance::find(machine_name.c_str());
        }
        std::stringstream ss;
		if (m) {
			m->sendMessageToReceiver( new Message(strdup(command.c_str())), m, false);
            if (m->_type == "LIST") {
                for (unsigned int i=0; i<m->parameters.size(); ++i) {
                    MachineInstance *entry = m->parameters[i].machine;
                    if (entry) m->sendMessageToReceiver(new Message(strdup(command.c_str())), entry);
                }
            }
            else if (m->_type == "REFERENCE" && m->locals.size()) {
                for (unsigned int i=0; i<m->locals.size(); ++i) {
                    MachineInstance *entry = m->locals[i].machine;
                    if (entry) m->sendMessageToReceiver(new Message(strdup(command.c_str())), entry);
                }
            }
			ss << command << " sent to " << m->getName() << std::flush;
			result_str = ss.str();
			return true;
		}
		else {
			ss << "Could not find machine "<<machine_name<<" for command " << command << std::flush;
		}
        error_str = ss.str();
        return false;
    }


    bool IODCommandData::run(std::vector<Value> &params) {
        //if (params.size() == 4) {
        MachineInstance *m = MachineInstance::find(params[1].asString().c_str());
        if (m && m->_type == "LIST") {
            
            for (unsigned int i=2; i<params.size(); ++i) {
                m->addParameter(params[i]);
            }
            
            result_str = "OK";
            return true;
        }
        else {
            std::string err ="Unknown device: ";
            err = err + params[1].asString() + "\n";
            error_str = err.c_str();
            return false;
        }
        /*}
         else {
         std::stringstream ss;
         ss << "Unrecognised parameters in ";
         std::ostream_iterator<std::string> out(ss, " ");
         std::copy(params.begin(), params.end(), out);
         ss << ".  Usage: PROPERTY property_name value";
         error_str = ss.str();
         return false;
         }
         */
    }

    bool IODCommandShowMessages::run(std::vector<Value> &params) {
        MessageLog *log = MessageLog::instance();
        // the CLEAR MESSAGES command is routed here by the client interface..
        if (params[0].asString() == "CLEAR") {
            MessageLog::instance()->purge();
            result_str = "OK";
            return true;
        }
        
        bool use_json = params.size() >= 2 && params[1].asString() == "JSON";
        long num = 0;
        unsigned int idx = 1;
        if (params.size() > idx && params[idx].asString() == "JSON") { use_json = true; ++idx; }
        if (params.size() > idx && params[idx].asInteger(num)) { ++idx; }
        
        cJSON *result = log->toJSON((unsigned int)num);
        if (result) {
            if (use_json) {
                char *text = cJSON_Print(result);
                result_str = text;
                free(text);
            }
            else {
                char *res = log->toString((unsigned int)num);
                result_str = res;
                free(res);
            }
            cJSON_Delete(result);
            return true;
        }
        else {
            error_str = "Failed to read the error log";
            return false;
        }
    }

    bool IODCommandNotice::run(std::vector<Value> &params) {
        std::stringstream msg;
        unsigned int i = 0;
        for (; i<params.size()-1; ++i) {
            msg << params.at(i) << " ";
        }
        msg << params.at(i);
        char *msg_str = strdup(msg.str().c_str());
        MessageLog::instance()->add(msg_str);
        free(msg_str);
        result_str = "OK";
        return true;
    }

    bool IODCommandQuit::run(std::vector<Value> &params) {
        program_done = true;
        std::stringstream ss;
        ss << "quitting ";
        std::ostream_iterator<std::string> oi(ss, " ");
        ss << std::flush;
        result_str = ss.str();
        return true;
    }

    bool IODCommandHelp::run(std::vector<Value> &params) {
        std::stringstream ss;
        ss 
		 << "Commands: \n"
		 << "DEBUG machine on|off\n"
		 << "DEBUG debug_group on|off\n"
         << "DESCRIBE machine_name [JSON]\n"
		 << "DISABLE machine_name\n"
		 << "EC command\n"
		 << "ENABLE machine_name\n"
		 << "GET machine_name\n"
		 << "LIST JSON\n"
		 << "LIST\n"
		 << "MASTER\n"
		 << "MODBUS EXPORT\n"
		 << "MODBUS group address new_value\n"
         << "MODBUS REFRESH\n"
		 << "PROPERTY machine_name property new_value\n"
		 << "QUIT\n"
		 << "RESUME machine_name\n"
		 << "SEND command\n"
		 << "SET machine_name TO state_name\n"
		 << "SLAVES\n"
		 << "TOGGLE output_name\n"
         << "ERRORS [JSON]\n"
		;
		std::string s = ss.str();
        result_str = s;
        return true;
    }

    bool IODCommandInfo::run(std::vector<Value> &params) {
        const char *device = device_name();
        if (!device) device = "localhost";
        const char *version = "version: 0.0";
        char *res = 0;
        if (params.size() >1 && params[1] == "JSON") {
            cJSON *info = cJSON_CreateArray();
            cJSON_AddStringToObject(info, "NAME", device);
            cJSON_AddStringToObject(info, "VERSION", version);
            res = cJSON_Print(info);
            cJSON_Delete(info);
            
        }
        else {
            res = (char *)malloc(100);
            snprintf(res,100, "DEVICE:\t%s\nVERSION\t%s\n", device, version);
        }
        if (res) {
            result_str = res;
            free(res);
            return true;
        }
        else {
            error_str = "No System Information available";
            return false;
        }
    }


    bool IODCommandTracing::run(std::vector<Value> &params) {
		if (params.size() != 2) {
			std::stringstream ss;
			ss << "usage: TRACING ON|OFF" << std::flush;
			std::string s = ss.str();
			error_str = s;
			return false;
		}
        if (params[1] == "ON") {
            enable_tracing(true);
        }
        else {
            enable_tracing(false);
        }
		result_str = "OK";
		return true;
    }

    bool IODCommandDebugShow::run(std::vector<Value> &params) {
		std::stringstream ss;
		ss << "Debug status: \n" << *LogState::instance() << "\n" << std::flush;
		std::string s = ss.str();
		result_str = s;
		return true;
	}

    bool IODCommandDebug::run(std::vector<Value> &params) {
		if (params.size() != 3) {
			std::stringstream ss;
			ss << "usage: DEBUG debug_group on|off\n\nDebug groups: \n" << *LogState::instance() << std::flush;
			std::string s = ss.str();
			error_str = s;
			return false;
		}
		int group = LogState::instance()->lookup(params[1].asString());
		if (group == 0) {
			std::map<std::string, MachineInstance *>::iterator found = machines.find(params[1].asString());
			if (found != machines.end()) {
				MachineInstance *mi = (*found).second;
				if (params[2] == "on" || params[2] == "ON") {
					mi->setDebug(true);
				}
				else {
					mi->setDebug(false);
				}
			}
			else {
				error_str = "Unknown debug group";
				return false;
			}
		}
		if (params[2] == "on" || params[2] == "ON") {
			LogState::instance()->insert(group);
			if (params[1] == "DEBUG_MODBUS") ModbusAddress::message("DEBUG ON");
		}
		else if (params[2] == "off" || params[2] == "OFF") {
			LogState::instance()->erase(group);
			if (params[1] == "DEBUG_MODBUS") ModbusAddress::message("DEBUG OFF");
		}
		else {
			error_str = "Please use 'on' or 'off' in all upper- or lowercase";
			return false;
		}
		result_str = "OK";
		return true;
	}

    bool IODCommandModbus::run(std::vector<Value> &params) {
		if (params.size() != 4) {
			error_str = "Usage: MODBUS group address value";
			std::cout << error_str <<  " " << params[1] << "\n";
			return false;
		}
		long group, address;
		if (!params[1].asInteger(group) || !params[2].asInteger(address)) {
			std::stringstream ss;
			ss << "malformed modbus command: " << params[0] <<" " << params[1] <<" " << params[2] <<" " << params[3];
			error_str = ss.str();
			MessageLog::instance()->add(ss.str().c_str());
			return false;
		}
		DBG_MODBUS << "modbus group: " << group << " addresss " << address << " value " << params[3] << "\n";
		ModbusAddress found = ModbusAddress::lookup((int)group, (int)address);
		if (found.getGroup() != ModbusAddress::none) {
			if (found.getOwner()) {
				// the address found will refer to the base address, so we provide the actual offset
				assert(address == found.getAddress());
    		if (params[3].kind == Value::t_integer) {
       		found.getOwner()->modbusUpdated(found, (int)(address - found.getAddress()), (int)params[3].iValue);
        }
        else if (params[3].kind == Value::t_bool) {
        	found.getOwner()->modbusUpdated(found, (int)(address - found.getAddress()), (params[3].bValue) ? 1 : 0);
				}
        else if (params[3].kind == Value::t_string || params[3].kind == Value::t_symbol) {
                long val;
                if (params[3].asInteger(val))
	       		found.getOwner()->modbusUpdated(found, (int)(address - found.getAddress()), (int)val);
					else
	       		found.getOwner()->modbusUpdated(found, (int)(address - found.getAddress()), params[3].sValue.c_str());
 				}
				else {
					std::stringstream ss;
					ss << "unexpected value type " << params[3].kind << " for modbus value\n";
					std::cout << ss.str() << "\n";
					error_str = ss.str();
					return false;
				}
			}
			else {
				DBG_MODBUS << "no owner for Modbus address " << found << "\n";
				error_str = "Modbus: ignoring unregistered address\n";
				std::cout << error_str << "\n";
				return false;
			}
		}
		else {
			std::stringstream ss;
			ss << "failed to find Modbus Address matching group: " << group << ", address: " << address;
			error_str = ss.str();
			std::cout << error_str << "\n";
			return false;
		}
		result_str = "OK";
		return true;
	}

    bool IODCommandModbusExport::run(std::vector<Value> &params) {

		const char *file_name = modbus_map();
		const char *backup_file_name = "modbus_mappings.bak";
		if (rename(file_name, backup_file_name)) {
			std::cerr << strerror(errno) << "\n";
		}
		std::list<MachineInstance*>::iterator m_iter = MachineInstance::begin();
		std::ofstream out(file_name);
		if (!out) {
			error_str = "not able to open mapping file for write";
			return false;
		}
		while (m_iter != MachineInstance::end()) {
			(*m_iter)->exportModbusMapping(out);
			m_iter++;
		}
		out.close();
		result_str = "OK";
		return true;
	}

    bool IODCommandPersistentState::run(std::vector<Value> &params) {
        cJSON *result = cJSON_CreateArray();
        std::list<MachineInstance *>::iterator m_iter;
        m_iter = MachineInstance::begin();
        while (m_iter != MachineInstance::end()) {
            MachineInstance *m = *m_iter++;
            if (m && (m->getValue("PERSISTENT") == "true") ) {
                std::string fnam = m->fullName();
                SymbolTableConstIterator props_i = m->properties.begin();
                while (props_i != m->properties.end()) {
                    std::pair<std::string, Value> item = *props_i++;
                    cJSON *json_item = cJSON_CreateArray();
                    cJSON_AddItemToArray(json_item, cJSON_CreateString(fnam.c_str()));
                    cJSON_AddItemToArray(json_item, cJSON_CreateString(item.first.c_str()));
                    MessageEncoding::addValueToJSONArray(json_item, item.second);
                    cJSON_AddItemToArray(result, json_item);
                }
            }
        }
        char *cstr_result = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
        result_str = cstr_result;
        free(cstr_result);
        return true;
    }

    bool IODCommandSchedulerState::run(std::vector<Value> &params) {
		std::stringstream ss;
		ss << "Status: " <<  Scheduler::instance()->getStatus() << "\n";
		if (!Scheduler::instance()->empty()) 
			ss << "next: " << *(Scheduler::instance()->next());
		result_str = ss.str();
		return true;
	}

    bool IODCommandModbusRefresh::run(std::vector<Value> &params) {
		std::list<MachineInstance*>::iterator m_iter = MachineInstance::begin();
		cJSON *result = cJSON_CreateArray();
		while (m_iter != MachineInstance::end()) {
			(*m_iter)->refreshModbus(result);
			m_iter++;
		}
		//std::string s(out.str());
        if (result) {
            char *r_str = cJSON_Print(result);
            result_str = r_str;
            free(r_str);
            cJSON_Delete(result);
            return true;
        }
        else {
            error_str = "Failed to load modbus initial data";
            return false;
        }
	}

    bool IODCommandPerformance::run(std::vector<Value> &params) {
        std::list<MachineInstance*>::iterator m_iter = MachineInstance::begin();
        cJSON *result = cJSON_CreateArray();
        while (m_iter != MachineInstance::end()) {
            MachineInstance *m = *m_iter++;
            if (m->stable_states_stats.getCount()) {
                cJSON *stat = cJSON_CreateArray();
                cJSON_AddItemToArray(stat, cJSON_CreateString(m->fullName().c_str()));
                cJSON_AddItemToArray(stat, cJSON_CreateString(m->stable_states_stats.getName().c_str()));
                m->stable_states_stats.reportArray(stat);
                cJSON_AddItemToArray(result, stat);
            }
            if ( m->message_handling_stats.getCount()) {
                cJSON *stat = cJSON_CreateArray();
                cJSON_AddItemToArray(stat, cJSON_CreateString(m->fullName().c_str()));
                cJSON_AddItemToArray(stat, cJSON_CreateString(m->message_handling_stats.getName().c_str()));
                m->message_handling_stats.reportArray(stat);
                cJSON_AddItemToArray(result, stat);
            }
        }
		{
            cJSON *stats = cJSON_CreateArray();
			Statistic::reportAll(stats);
			cJSON_AddItemToArray(result, stats);
		}
		
        //std::string s(out.str());
        if (result) {
            char *r_str = cJSON_Print(result);
            result_str = r_str;
            free(r_str);
            cJSON_Delete(result);
            return true;
        }
        else {
            error_str = "Failed to load modbus initial data";
            return false;
        }
    }

	bool IODCommandChannels::run(std::vector<Value> &params) {
		std::map< std::string, Channel* > *channels = Channel::channels();
		if (!channels) {result_str = "No channels"; return true; }
		
		std::string result;
		std::map<std::string, Channel*>::iterator iter = channels->begin();
		while (iter != channels->end()) {
			result += (*iter++).first + "\n";
		}
		result_str = result;
		return true;
	}

    bool IODCommandChannel::run(std::vector<Value> &params) {
        if (params.size() >= 2) {
            Value ch_name = params[1];
            Channel *chn = Channel::find(ch_name.asString());
            if (chn) {
                size_t n = params.size();
                if (n == 3) {
                    if (params[2] == "REMOVE") {
                        delete chn;
                        result_str = "OK";
                        return true;
                    }
                }
                if (n == 5 && params[2] == "ADD" && params[3] == "MONITOR") {
                    chn->addMonitor(params[4].asString().c_str());
                    result_str = "OK";
                    return true;
                }
                if (n == 6 && params[2] == "ADD" && params[3] == "MONITOR" && params[4] == "PATTERN") {
                    chn->addMonitorPattern(params[5].asString().c_str());
                    result_str = "OK";
                    return true;
                }
                if (n == 7 && params[2] == "ADD" && params[3] == "MONITOR" && params[4] == "PROPERTY") {
                    chn->addMonitorProperty(params[5].asString().c_str(),params[6]);
                    result_str = "OK";
                    return true;
                }
                if (n == 5 && params[2] == "REMOVE" && params[3] == "MONITOR") {
                    chn->removeMonitor(params[4].asString().c_str());
                    result_str = "OK";
                    return true;
                }
                if (n == 6 && params[2] == "REMOVE" && params[3] == "MONITOR" && params[4] == "PATTERN") {
                    chn->removeMonitorPattern(params[5].asString().c_str());
                    result_str = "OK";
                    return true;
                }
                if (n == 7 && params[2] == "REMOVE" && params[3] == "MONITOR" && params[5] == "PROPERTY") {
                    chn->removeMonitorProperty(params[5].asString().c_str(),params[6]);
                    result_str = "OK";
                    return true;
                }
                
            }
            else {
                ChannelDefinition *defn = ChannelDefinition::find(ch_name.asString().c_str());
                if (!defn) {
                    error_str = MessageEncoding::encodeError("No such channel");
                    return false;
                }
				
                chn = Channel::findByType(ch_name.asString());
                if (!chn) {
                    int port = Channel::uniquePort();
                    while (true) {
                        try {
                            chn = defn->instantiate(port);
							chn->start();
							chn->enable();
                            break;
                        }
                        catch (zmq::error_t err) {
                            if (zmq_errno() == EADDRINUSE) {
                                continue;
                            }
                            error_str = zmq_strerror(zmq_errno());
                            std::cerr << error_str << "\n";
                            exit(1);
                        }
                    }
                }
            }
            cJSON *res_json = cJSON_CreateObject();
            cJSON_AddNumberToObject(res_json, "port", chn->getPort());
            cJSON_AddStringToObject(res_json, "name", chn->getName().c_str());
            char *res = cJSON_Print(res_json);
            result_str = res;
            free(res);
            free(res_json);
            return true;
        }
        std::stringstream ss;
        ss << "usage: CHANNEL name [ REMOVE| ADD MONITOR [ ( PATTERN string | PROPERTY string string ) ]  ]";
        for (unsigned int i=0; i<params.size()-1; ++i) {
            ss<< params[i] << " ";
        }
        ss << params[params.size()-1];
        char *msg = strdup(ss.str().c_str());
        error_str = msg;
        free(msg);
        return false;
    }

    bool IODCommandUnknown::run(std::vector<Value> &params) {
        if (params.size() == 0) {
            result_str = "OK";
            return true;
        }
        std::stringstream ss;
        for (unsigned int i=0; i<params.size()-1; ++i) {
            ss<< params[i] << " ";
        }
        ss << params[params.size()-1];
        char *msg = strdup(ss.str().c_str());
        
        if (message_handlers.count(msg)) {
            const std::string &name = message_handlers[ss.str()];
            MachineInstance *m = MachineInstance::find(name.c_str());
            if (m) {
                m->sendMessageToReceiver(new Message(msg),m);
                result_str = "OK";
                return true;
            }
        }
        ss << ": Unknown command: ";
        error_str = msg;
        free(msg);
        return false;
    }

void sendMessage(zmq::socket_t &socket, const char *message) {
    const char *msg = (message) ? message : "";
    size_t len = strlen(msg);
    zmq::message_t reply (len);
    memcpy ((void *) reply.data (), msg, len);
    while (true) {
        try {
            socket.send (reply);
            //std::cout << "sent: " << message << "\n";
            break;
        } catch (zmq::error_t e) {
            if (errno == EAGAIN) continue;
            std::cerr << "Error: " << errno << " " << zmq_strerror(errno) << "\n";
        }
    }
}

