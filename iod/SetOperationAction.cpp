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

#include "SetOperationAction.h"
#include "MachineInstance.h"
#include "Logger.h"

SetOperationActionTemplate::SetOperationActionTemplate(Value a, Value b,
                                                       Value destination, Value property, SetOperation op)
    : src_a_name(a.asString()), src_b_name(b.asString()), dest_name(destination.asString()), operation(op) {
}

SetOperationActionTemplate::~SetOperationActionTemplate() {
}
                                           
Action *SetOperationActionTemplate::factory(MachineInstance *mi) {
	return new SetOperationAction(mi, this);
}

SetOperationAction::SetOperationAction(MachineInstance *m, const SetOperationActionTemplate *dat)
    : Action(m), source_a(dat->src_a_name), source_b(dat->src_b_name),
        dest(dat->dest_name), dest_machine(0),  operation(dat->operation) {
}

SetOperationAction::SetOperationAction() : dest_machine(0) {
}

std::ostream &SetOperationAction::operator<<(std::ostream &out) const {
	return out << "Set Operation " << source_a << ", " << source_b << " to " << dest << "\n";
}

class SetOperationException : public std::exception {
    
};

bool CompareValues(Value a, Value &b){
    if (a == SymbolTable::Null || b == SymbolTable::Null) throw SetOperationException();
    return a == b;
}

bool CompareSymbolAndValue(MachineInstance*scope, Value &sym, std::string &prop, Value &b){
    MachineInstance *mi = scope->lookup(sym);
    if (!mi) throw new SetOperationException();
    return CompareValues(mi->getValue(prop), b);
}

Action::Status SetOperationAction::run() {
	owner->start(this);
    try {
        if (!source_a_machine) source_a_machine = owner->lookup(source_a);
        if (!source_b_machine) source_b_machine = owner->lookup(source_b);
        if (!dest_machine) dest_machine = owner->lookup(dest);
        if (dest_machine && dest_machine->_type == "LIST") {
            if (operation == soIntersect) {
                for (unsigned int i=0; i < source_a_machine->parameters.size(); ++i) {
                    Value &a(source_a_machine->parameters.at(i).val);
                    for (unsigned int j = 0; j < source_b_machine->parameters.size(); ++j) {
                        Value &b(source_b_machine->parameters.at(j).val);
                        Value v1(a);
                        if (v1.kind == Value::t_symbol && (b.kind == Value::t_string || b.kind == Value::t_integer)) {
                            if (CompareSymbolAndValue(owner, v1, property_name, b)) {
                                dest_machine->addParameter(a);
                            }
                        }
                        else if (b.kind == Value::t_symbol && (v1.kind == Value::t_string || v1.kind == Value::t_integer)) {
                            if (CompareSymbolAndValue(owner, b, property_name, v1))
                                dest_machine->addParameter(a);
                        }
                        else if (a == b) dest_machine->addParameter(a);
                    }
                }
                status = Complete;
            }
            else
                status = Failed;
        }
        else
            status = Failed;
    }
    catch (SetOperationException &e) {
        status = Failed;
    }
    owner->stop(this);
	return status;
}

Action::Status SetOperationAction::checkComplete() {
	if (status == Complete || status == Failed) return status;
	if (this != owner->executingCommand()) {
		DBG_MSG << "checking complete on " << *this << " when it is not the top of stack \n";
	}
	else {
		status = Complete;
		owner->stop(this);
	}
	return status;
}
