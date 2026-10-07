#include "hirari/shell.hpp"
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
namespace hirari {
namespace {
std::string identity(seed::Runtime& r,seed::Ptr p) {
    if(r.is_name(p))p=r.as_name(p)->target;
    if(r.is_cell(p))return r.as_cell(p)->symbol->text;
    if(r.is_symbol(p))return r.as_symbol(p)->text;
    if(r.is_literal(p))return r.as_literal(p)->bytes;
    throw std::runtime_error("expected a binding or command identity");
}
const std::map<std::string,std::string> documentation{
    {"help","help [SUBJECT] | SUBJECT help | ? SUBJECT\nInspect definitions, command origins or retained CLR members without executing the subject."},
    {"?","? SUBJECT\nReflective help. SUBJECT help is the same inspection. Use demand SUBJECT to evaluate."},
    {"demand","demand SUBJECT\nEvaluate the subject using Seed's reduction rules."},
    {":=","NAME := EXPRESSION\nBind a retained, lazy program form. EXPRESSION = NAME is its inverse orientation."},
    {"=","EXPRESSION = NAME\nInverse binding. Equivalent to NAME := EXPRESSION."},
    {"|","PRODUCER | CONSUMER\nEquivalent to CONSUMER PRODUCER and CONSUMER(PRODUCER). Native stages use live byte streams; ordinary values retain structure."},
    {":", "B : A\nEquivalent to A () B. Attached :name is a semantic Name and is a different form."},
    {"@","@ NAME\nInspect the stable binding cell; this does not read its head."},
    {"*","* NAME | * (@ NAME)\nRead/reduce the cell's current head."},
    {"if","if CONDITION CONSEQUENT [ALTERNATE]\nOnly the chosen branch evaluates. Process success uses exit status."},
    {"for","for :ITEM :(VALUES ...) BODY\nIterate structured values. The previous ITEM binding is restored, including on failure."},
    {"truth","truth VALUE\nTests Unit/false/zero/empty values, or a process's exit status."},
    {"source","source \"FILE.seed\" [ARGUMENTS ...]\nEvaluate definitions in the caller's scope. File-relative includes preserve argument frames."},
    {"run","run \"FILE.seed\" [ARGUMENTS ...]\nInvoke a Seed program. Restore caller definitions, cwd and world selection afterward. External effects remain."},
    {"arg","arg INDEX\nRead a zero-based structured program argument."},
    {"args","args\nReturn a reified product of all current program arguments."},
    {"argc","argc\nNumber of arguments in the active Seed program."},
    {"script-directory","script-directory\nDirectory of the currently executing .seed program."},
    {"path","path \"ROOT\" \"SEGMENT\" ...\nJoin filesystem paths without rewriting arbitrary program arguments."},
    {"apply","apply :PROGRAM :(ARGUMENTS ...)\nInvoke a reified program with a structured argument product. apply :(exec \"program\") (args) forwards script arguments."},
    {"exec","exec \"PROGRAM PATH\" [ARGUMENTS ...]\nLaunch an executable with argument values. No shell-text reparsing."},
    {"cd","cd [\"PATH\"]\nChange the filesystem working directory. Without an argument, use the user profile."},
    {"pwd","pwd\nShow the filesystem working directory."},
    {"exit","exit\nEnd Hirari. Skunkworks and authored Unity objects remain alive."},
    {"quit","quit\nEnd Hirari. Equivalent to exit."},
    {"world","world open \"ROOT\" | world status\nRead a generic world.toml descriptor and invoke its Seed entry. Failures restore previous context."},
    {"unity","unity connect | status | disconnect\nunity type \"CLR.TYPE\" | find \"ROOT/NAME\"\nunity new TYPE ARGS | new-exact TYPE :(SIGNATURE) ARGS\nunity component OBJECT TYPE\nunity get TARGET \"MEMBER\" | set TARGET \"MEMBER\" VALUE\nunity call TARGET \"METHOD\" ARGS\nunity call-exact TARGET \"METHOD\" :(SIGNATURE) ARGS\nunity call-generic TARGET \"METHOD\" :(TYPEARGS) ARGS\nunity bind :NAME (OPERATION)\nunity batch :((OPERATION ...) ...) | at RESULTS INDEX\nunity help TARGET [\"MEMBER\" OFFSET COUNT]\nunity wait REQUEST [\"COMPLETION_MEMBER\" TIMEOUT_MS]\nunity release HANDLE\nReturned objects/arrays remain in Mono; the shell carries identity. Unit () maps to CLR null. Releasing a handle does not destroy the resource. Disconnect drops this connection's handles."},
    {"vocab","vocab status | list | history :NAME\nvocab save :NAME [\"DESCRIPTION\"]\nvocab restore :NAME REVISION | forget :NAME\nvocab use \"DIRECTORY\"\nDurable structural definitions with revision history. Interactive top-level definitions save automatically; script scopes remain transient. Live CLR handles cannot be saved. HIRARI_STATE selects the user store; '-' disables it."}
};
}
seed::Ptr Shell::help(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto terms=r.terms(relation);if(terms.empty())return relation;
    bool prefix=r.is_symbol(terms.front()) && (r.as_symbol(terms.front())->text=="help" || r.as_symbol(terms.front())->text=="?");
    bool suffix=terms.size()>1 && r.is_symbol(terms.back()) && r.as_symbol(terms.back())->text=="help";
    if(!prefix && !suffix)return relation;
    auto& shell=*static_cast<Shell*>(context);
    auto finish=[&](std::string text){auto value=r.arena().make_literal(text);shell.descriptions_.insert(value);return static_cast<seed::Ptr>(value);};
    if(terms.size()==1){std::ostringstream out;out<<"Hirari: programs, values and live capabilities\n";for(const auto& [name,text]:documentation)out<<name<<"  ";out<<"\nUse ? COMMAND or COMMAND help. Native --help is invoked explicitly.\n";return finish(out.str());}
    std::vector<seed::Ptr> subjects(prefix?terms.begin()+1:terms.begin(),prefix?terms.end():terms.end()-1);
    auto subject=subjects.size()==1?subjects.front():r.product(subjects);
    std::ostringstream out;out<<r.print(subject)<<'\n';
    std::unordered_set<seed::Ptr> seen;
    for(unsigned depth=0;depth<32;++depth) {
        if(!seen.insert(subject).second){out<<"cyclic definition\n";break;}
        if(auto ref=shell.unity_.foreign(subject)){
            if(ref->value.kind==wire::ValueKind::Object || ref->value.kind==wire::ValueKind::Type)out<<shell.unity_.describe(subject);
            else out<<"CLR primitive: "<<r.print(subject)<<'\n';break;
        }
        if(r.is_cell(subject)){out<<"binding cell revision "<<r.as_cell(subject)->revision<<'\n';subject=r.as_cell(subject)->symbol;continue;}
        if(r.is_name(subject)){
            auto target=r.as_name(subject)->target;if(!r.is_symbol(target)){out<<"reified value: "<<r.print(target)<<'\n';break;}subject=target;
        }
        if(r.is_relation(subject)){out<<"retained program form: "<<r.print(subject)<<'\n';auto t=r.terms(r.as_node(subject));if(t.empty())break;subject=t.front();continue;}
        if(!r.is_symbol(subject)){out<<"value: "<<r.print(subject)<<'\n';break;}
        auto symbol=r.as_symbol(subject);auto found=documentation.find(symbol->text);
        if(found!=documentation.end()){out<<found->second<<'\n';break;}
        if(auto cell=r.find_cell(symbol);cell && cell->head){
            out<<"binding revision "<<cell->revision<<": "<<symbol->text<<" := "<<r.print(cell->head)<<'\n';
            if(shell.vocabulary_)out<<shell.vocabulary_->annotation(symbol->text)<<'\n';subject=cell->head;continue;
        }
        auto executable=ProcessHost::executable_path(symbol->text);
        if(!executable.empty()){out<<"native executable: "<<executable<<"\nUse exec \""<<executable<<"\" \"--help\" for its own documentation.\n";break;}
        auto path=shell.find_script(symbol->text);
        if(!path.empty()){
            out<<"Seed program: "<<path_text(path)<<'\n';std::istringstream lines(read_source(path));std::string line;unsigned count=0;
            while(count++<40 && std::getline(lines,line)){auto first=line.find_first_not_of(" \t\r");if(first==std::string::npos)continue;if(line[first]!='#')break;out<<line.substr(first)<<'\n';}break;
        }
        out<<"unresolved identity; define it or place a program on HIRARI_PATH/PATH\n";break;
    }
    return finish(out.str());
}
seed::Ptr Shell::vocabulary(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto terms=r.terms(relation);if(terms.empty() || !r.is_symbol(terms[0]) || r.as_symbol(terms[0])->text!="vocab")return relation;
    auto& shell=*static_cast<Shell*>(context);
    if(terms.size()<2 || !r.is_symbol(terms[1]))throw std::runtime_error("use vocab help for namespace operations");
    auto op=r.as_symbol(terms[1])->text;
    if(op=="use"){
        if(terms.size()!=3)throw std::runtime_error("usage: vocab use \"DIRECTORY\"");
        auto value=r.reduce(terms[2]);if(!r.is_literal(value))throw std::runtime_error("vocabulary directory requires quoted text");
        auto next=std::make_unique<Vocabulary>(text_path(r.as_literal(value)->bytes));next->load(r);shell.vocabulary_=std::move(next);return r.arena().unit();
    }
    if(!shell.vocabulary_)throw std::runtime_error("durable vocabulary disabled; use vocab use \"DIRECTORY\"");
    auto& store=*shell.vocabulary_;
    if(op=="status" && terms.size()==2)return r.arena().make_literal(path_text(store.directory()));
    if((op=="list" && terms.size()==2) || (op=="history" && terms.size()==3)){
        auto p=r.arena().make_literal(store.describe(r,op=="history"?identity(r,terms[2]):std::string{},op=="history"));shell.descriptions_.insert(p);return p;
    }
    if(op=="save" && (terms.size()==3 || terms.size()==4)){
        std::string description;if(terms.size()==4){auto p=r.reduce(terms[3]);if(!r.is_literal(p))throw std::runtime_error("annotation requires text");description=r.as_literal(p)->bytes;}
        store.save(r,identity(r,terms[2]),description,[&](auto p){return shell.unity_.foreign(p)!=nullptr;});return r.arena().unit();
    }
    if(op=="forget" && terms.size()==3){store.forget(r,identity(r,terms[2]));return r.arena().unit();}
    if(op=="restore" && terms.size()==4){auto s=r.print(r.reduce(terms[3]));if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("revision requires a positive integer");store.restore(r,identity(r,terms[2]),std::stoull(s));return r.arena().unit();}
    throw std::runtime_error("unknown vocabulary operation; use vocab help");
}
}
