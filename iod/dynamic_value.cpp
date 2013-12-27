#include <iostream>
#include <iterator>
#include "symboltable.h"
#include <numeric>
#include <functional>
#include <sstream>
#include "Logger.h"
#include <boost/foreach.hpp>
#include <utility>
#include "DebugExtra.h"
#include "MachineInstance.h"
#include "dynamic_value.h"


DynamicValue *DynamicValue::clone() const { return new DynamicValue(); }

std::ostream &DynamicValue::operator<<(std::ostream &out ) const {
    return out << "<dynamic value> (" << last_result << ")";
}
std::ostream &operator<<(std::ostream &out, const DynamicValue &val) { return val.operator<<(out); }

DynamicValue *PopListBackValue::clone() const { return new PopListBackValue(*this); }
std::ostream &PopListBackValue::operator<<(std::ostream &out ) const {
    if (remove_from_list)
        return out << " TAKE LAST FROM " << machine_list_name;
    else
        return out << " LAST OF " << machine_list_name;
}
std::ostream &operator<<(std::ostream &out, const PopListBackValue &val) { return val.operator<<(out); }

DynamicValue *PopListFrontValue::clone() const { return new PopListFrontValue(*this); }
std::ostream &PopListFrontValue::operator<<(std::ostream &out ) const {
    if (remove_from_list)
        return out << " TAKE FIRST FROM " << machine_list_name;
    else
        return out << " FIRST OF " << machine_list_name;
}
std::ostream &operator<<(std::ostream &out, const PopListFrontValue &val) { return val.operator<<(out); }

DynamicValue *AssignmentValue::clone() const { return new AssignmentValue(*this); }
std::ostream &AssignmentValue::operator<<(std::ostream &out ) const {
    return out << dest_name << " BECOMES " << src;
}
std::ostream &operator<<(std::ostream &out, const AssignmentValue &val) { return val.operator<<(out); }

DynamicValue *ItemAtPosValue::clone() const { return new ItemAtPosValue(*this); }
std::ostream &ItemAtPosValue::operator<<(std::ostream &out ) const {
    return out << "ITEM " << index << " OF " << machine_list_name;
}
std::ostream &operator<<(std::ostream &out, const ItemAtPosValue &val) { return val.operator<<(out); }

DynamicValue *AnyInValue::clone() const { return new AnyInValue(*this); }
std::ostream &AnyInValue::operator<<(std::ostream &out ) const {
    return out << "ANY " << machine_list_name << " IN " << state;
}
std::ostream &operator<<(std::ostream &out, const AnyInValue &val) { return val.operator<<(out); }


DynamicValue *AllInValue::clone() const { return new AllInValue(*this); }
std::ostream &AllInValue::operator<<(std::ostream &out ) const {
    return out << "ALL " << machine_list_name << " ARE " << state;
}
std::ostream &operator<<(std::ostream &out, const AllInValue &val) { return val.operator<<(out); }

DynamicValue *CountValue::clone() const { return new CountValue(*this); }
std::ostream &CountValue::operator<<(std::ostream &out ) const {
    return out << "COUNT " << state << " FROM " << machine_list_name << " (" << last_result << ")";
}
std::ostream &operator<<(std::ostream &out, const CountValue &val) { return val.operator<<(out); }

DynamicValue *IncludesValue::clone() const { return new IncludesValue(*this); }
std::ostream &IncludesValue::operator<<(std::ostream &out ) const {
    return out << machine_list_name << " INCLUDES " << entry_name;
}
std::ostream &operator<<(std::ostream &out, const IncludesValue &val) { return val.operator<<(out); }

DynamicValue *SizeValue::clone() const { return new SizeValue(*this); }
std::ostream &SizeValue::operator<<(std::ostream &out ) const {
    return out << "SIZE OF " << machine_list_name;
}
std::ostream &operator<<(std::ostream &out, const SizeValue &val) { return val.operator<<(out); }

DynamicValue *BitsetValue::clone() const { return new BitsetValue(*this); }
std::ostream &BitsetValue::operator<<(std::ostream &out ) const {
    return out << "BITSET FROM " << machine_list << state;
}
std::ostream &operator<<(std::ostream &out, const BitsetValue &val) { return val.operator<<(out); }

Value DynamicValue::operator()(MachineInstance*) { return SymbolTable::False; }

Value AssignmentValue::operator()(MachineInstance *mi) {
    if (src.kind == Value::t_symbol)
        last_result = mi->getValue(src.sValue);
    else
        last_result = src;
    mi->setValue(dest_name, last_result);
    return last_result;
}

Value AnyInValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list) { last_result = false; return last_result; }
    for (unsigned int i=0; i<machine_list->parameters.size(); ++i) {
        if (!machine_list->parameters[i].machine) mi->lookup(machine_list->parameters[i]);
        if (!machine_list->parameters[i].machine) continue;
        
        if (state == machine_list->parameters[i].machine->getCurrent().getName()) { last_result = true; return last_result; }
    }
    last_result = false; return last_result;
}
Value AllInValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list) { last_result = false; return last_result; }
    if (machine_list->parameters.size() == 0) { last_result = false; return last_result; }
    for (unsigned int i=0; i<machine_list->parameters.size(); ++i) {
        if (!machine_list->parameters[i].machine) mi->lookup(machine_list->parameters[i]);
        if (!machine_list->parameters[i].machine) continue;
        
        if (state != machine_list->parameters[i].machine->getCurrent().getName()) { last_result = false; return last_result; }
    }
    last_result = true; return last_result;
}

Value CountValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list) return false;
    if (machine_list->parameters.size() == 0) return 0;
    int result = 0;
    for (unsigned int i=0; i<machine_list->parameters.size(); ++i) {
        if (!machine_list->parameters[i].machine) mi->lookup(machine_list->parameters[i]);
        if (!machine_list->parameters[i].machine) continue;
        
        if (state == machine_list->parameters[i].machine->getCurrent().getName()) ++result;
    }
    last_result = result;
    return last_result;
}

Value SizeValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list)  { last_result = false; return last_result; }
    last_result = machine_list->parameters.size();
    return last_result;
}

Value PopListBackValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list)  { last_result = false; return last_result; }
    long i = machine_list->parameters.size() - 1;
    last_result = false;
    if (i>=0) {
        last_result = machine_list->parameters[i].val;
        if (remove_from_list){
            machine_list->parameters.pop_back();
            machine_list->setNeedsCheck();
        }
    }
    return last_result;
}

Value PopListFrontValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list)  { last_result = false; return last_result; }
    last_result = false;
    if (machine_list->parameters.size()) {
        last_result = machine_list->parameters[0].val;
        if (remove_from_list){
            machine_list->parameters.erase(machine_list->parameters.begin());
            if (machine_list->_type == "LIST") {
                machine_list->setNeedsCheck();
            }
        }
    }
    return last_result;
}

Value IncludesValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list)  { last_result = false; return last_result; }
    for (unsigned int i=0; i<machine_list->parameters.size(); ++i) {
        if (entry_name == machine_list->parameters[i].val.asString() || entry_name == machine_list->parameters[i].real_name)  { last_result = true; return last_result; }
    }
    last_result = false; return last_result;
}

Value ItemAtPosValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list)  { last_result = false; return last_result; }
    if (machine_list->parameters.size()) {
        long idx = 0;
        if (index.kind == Value::t_symbol)
            if (mi->getValue(index.sValue).asInteger(idx)) {
                if (idx>=0 && idx < machine_list->parameters.size()) {
                    last_result = machine_list->parameters[idx].val;
                    if (remove_from_list) {
                        machine_list->parameters.erase(machine_list->parameters.begin()+idx);
                        if (machine_list->_type == "LIST") {
                            machine_list->setNeedsCheck();
                        }
                    }
                    return last_result;
                }
            }
        }
    else {
        long idx = 0;
        if (index.asInteger(idx)) {
            last_result = machine_list->parameters[idx].val;
            return last_result;
        }
    }
    
    last_result = false; return last_result;
}

Value BitsetValue::operator()(MachineInstance *mi) {
    if (machine_list == NULL) machine_list = mi->lookup(machine_list_name);
    if (!machine_list)  { last_result = false; return last_result; }
    unsigned long val = 0;
    for (unsigned int i=0; i<machine_list->parameters.size(); ++i) {
        MachineInstance *entry = machine_list->parameters[i].machine;
        val *= 2;
        if (entry) {
            if (state == entry->getCurrentStateString())  {
                val += 1;
            }
        }
    }
    last_result = val;
    return last_result;
}

DynamicValue *EnabledValue::clone() const { return new EnabledValue(*this); }
Value EnabledValue::operator()(MachineInstance *mi) {
    if (machine == NULL) machine = mi->lookup(machine_name);
    if (!machine)  { last_result = false; return last_result; }
    last_result = machine->enabled();
    return last_result;
}
std::ostream &EnabledValue::operator<<(std::ostream &out ) const {
    return out << machine_name << " ENABLED ";
}
std::ostream &operator<<(std::ostream &out, const EnabledValue &val) { return val.operator<<(out); }


DynamicValue *DisabledValue::clone() const { return new DisabledValue(*this); }
Value DisabledValue::operator()(MachineInstance *mi) {
    if (machine == NULL) machine = mi->lookup(machine_name);
    if (!machine)  { last_result = false; return last_result; }
    last_result = !machine->enabled();
    return last_result;
}
std::ostream &DisabledValue::operator<<(std::ostream &out ) const {
    return out << machine_name << " DISABLED ";
}
std::ostream &operator<<(std::ostream &out, const DisabledValue &val) { return val.operator<<(out); }


DynamicValue *CastValue::clone() const { return new CastValue(*this); }
Value CastValue::operator()(MachineInstance *mi) {
    Value val = mi->getValue(property);
    if (kind == "STRING")
        last_result = val.asString();
    else if (kind == "NUMBER") {
        long lValue = 0;
        if (val.asInteger(lValue))
            last_result = lValue;
        else
            last_result = false;
        return last_result;
    }
    return last_result;
}
std::ostream &CastValue::operator<<(std::ostream &out ) const {
    return out << "CAST(" << property << "," << kind << ") ";
}
std::ostream &operator<<(std::ostream &out, const CastValue &val) { return val.operator<<(out); }


