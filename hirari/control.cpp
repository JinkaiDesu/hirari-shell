#include "hirari/control.hpp"
#include <chrono>
#include <cmath>
#include <thread>
namespace hirari {
namespace {
using Clock=std::chrono::steady_clock;
bool head_is(seed::Runtime& r,seed::Node* relation,std::string_view name) {
    auto terms=r.terms(relation);
    return !terms.empty() && r.is_symbol(terms.front()) && r.as_symbol(terms.front())->text==name;
}
seed::Ptr boolean(seed::Runtime& r,bool value){return r.arena().make_name(r.intern(value?"true":"false"));}
// Demand a deferred operand exactly as `if` demands a selected branch.
seed::Ptr demand(seed::Runtime& r,seed::Ptr term){return r.reduce(r.product({r.intern("if"),boolean(r,true),term}));}
// Decide a condition exactly as `if` does, including program exit status.
bool holds(seed::Runtime& r,seed::Ptr term){return r.truth(r.reduce(r.product({r.intern("if"),term,boolean(r,true),boolean(r,false)})));}
seed::Ptr record(seed::Runtime& r,const std::vector<std::pair<const char*,seed::Ptr>>& fields) {
    std::vector<seed::Ptr> pairs;
    for(const auto& [key,value]:fields)pairs.push_back(r.arena().make_name(r.product({r.arena().make_name(r.intern(key)),value})));
    return r.arena().make_name(r.product({r.arena().make_name(r.intern("record")),r.arena().make_name(r.product(pairs))}));
}
}

Control::Control(seed::Runtime& runtime,Host& host):host_(host) {
    runtime.define("while",&Control::loop,this);
    runtime.define("sleep",&Control::pause,this);
    runtime.define("fail",&Control::fail,this);
    runtime.define("attempt",&Control::attempt,this);
}

seed::Ptr Control::loop(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    if(!head_is(r,relation,"while"))return relation;
    auto terms=r.terms(relation);
    if(terms.size()!=3)throw std::runtime_error("while expects a condition and a body");
    auto& self=*static_cast<Control*>(context);
    // Every iteration is a separate bounded reduction, so repetition count never
    // deepens the native stack. `exit` unwinds through the loop like any form.
    for(;;) {
        if(self.host_.interrupted())throw Interruption("while interrupted");
        if(!holds(r,terms[1]))return r.arena().unit();
        demand(r,terms[2]);
    }
}

seed::Ptr Control::pause(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    if(!head_is(r,relation,"sleep"))return relation;
    auto terms=r.terms(relation);
    if(terms.size()!=2)throw std::runtime_error("sleep SECONDS");
    auto value=r.reduce(terms[1]);
    if(!r.is_number(value))throw std::runtime_error("sleep requires a number of seconds");
    auto number=r.as_number(value);
    double seconds=r.arena().is(value,seed::HostKind::Integer)?static_cast<double>(number->integer):number->real;
    if(!std::isfinite(seconds)||seconds<0||seconds>86400)throw std::runtime_error("sleep accepts 0..86400 seconds");
    auto& self=*static_cast<Control*>(context);
    auto deadline=Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    while(Clock::now()<deadline) {
        if(self.host_.interrupted())throw Interruption("sleep interrupted");
        std::this_thread::sleep_for(std::min<Clock::duration>(deadline-Clock::now(),std::chrono::milliseconds(20)));
    }
    return r.arena().unit();
}

seed::Ptr Control::fail(seed::Runtime& r,seed::Node* relation,seed::Ptr) {
    if(!head_is(r,relation,"fail"))return relation;
    auto terms=r.terms(relation);std::string message;
    for(std::size_t i=1;i<terms.size();++i) {
        auto value=r.reduce(terms[i]);
        if(i>1)message+=' ';
        message+=r.is_literal(value)?r.as_literal(value)->bytes:r.print(value);
    }
    throw std::runtime_error(message.empty()?"operation failed":message);
}

seed::Ptr Control::attempt(seed::Runtime& r,seed::Node* relation,seed::Ptr) {
    if(!head_is(r,relation,"attempt"))return relation;
    auto terms=r.terms(relation);
    if(terms.size()!=2)throw std::runtime_error("attempt expects one expression");
    auto unit=r.arena().unit();seed::Ptr value;
    try {value=demand(r,terms[1]);}
    // Cancellation and `exit` are not operation failures; they keep unwinding.
    catch(const Interruption&){throw;}
    catch(const std::exception& error) {
        return record(r,{{"status",r.arena().make_literal("error")},{"value",unit},{"error",r.arena().make_literal(error.what())},{"exit_status",unit}});
    }
    ProcessResult process;
    if(ProcessHost::unpack(r,value,process)) {
        auto status=r.arena().make_number(std::to_string(process.status));
        if(process.status!=0)
            return record(r,{{"status",r.arena().make_literal("failed")},{"value",value},{"error",r.arena().make_literal(process.err)},{"exit_status",status}});
        return record(r,{{"status",r.arena().make_literal("ok")},{"value",value},{"error",unit},{"exit_status",status}});
    }
    return record(r,{{"status",r.arena().make_literal("ok")},{"value",value},{"error",unit},{"exit_status",unit}});
}
}
