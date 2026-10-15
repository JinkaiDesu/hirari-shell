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
    {"help","help [-IDENTITY] | IDENTITY help | ? -IDENTITY\nRead descriptions, arguments, origins and binding revisions without executing the subject or extracting its body. Use inspect -IDENTITY for executable structure."},
    {"?","? -IDENTITY\nRead semantic metadata. IDENTITY help is the same request. Attached form? requests the form's produced answer and does not invoke help."},
    {"inspect","inspect -IDENTITY\nExplicitly inspect retained executable structure, bindings, origins and provider identity. The subject is not evaluated. Provider-specific inspection is invoked explicitly."},
    {"demand","demand SUBJECT | SUBJECT?\nEvaluate the subject through its provider. A bare callable runs with no arguments. The answer retains its value type and identity; it is not automatically Boolean."},
    {":=","NAME := EXPRESSION\nBind a retained, lazy program form. EXPRESSION = NAME is its inverse orientation."},
    {"=","EXPRESSION = NAME\nInverse binding. Equivalent to NAME := EXPRESSION."},
    {":", "BODY : BINDER\nEquivalent to BINDER() BODY: an abstraction waiting for an argument. Application substitutes the argument for free binder occurrences, with intentional capture and no alpha-renaming. This does not assert a type. Attached -name is a semantic Name and is a different form."},
    {"@","@ NAME\nInspect the stable binding cell; this does not read its head."},
    {"*","* NAME | * (@ NAME)\nRead/reduce the cell's current head."},
    {"_","_\nFresh anonymous position. Never a binding, implicit pipe hole, or previous result. As an abstraction binder it discards the argument."},
    {"&&","A && B | && A B\nDemand B only if A succeeds. Otherwise retain A, including its process status. Equal precedence with ||; chains associate left."},
    {"||","A || B | || A B\nRetain successful A; otherwise demand B. Equal precedence with &&; chains associate left. -or names this Program."},
    {"<","A < B | < A B\nStrict ordering of numeric literals or strings. Reverse operands for the inverse. -lt names this Program."},
    {"<=","A <= B | <= A B\nOrdering or equal value. -le names this Program; semantic classification belongs to providers. Equality of two strings or numbers is (A <= B) && (B <= A); no external test program is needed."},
    {">","VALUE > DESTINATION\nRoute to a file path or callable sink. Native stdout streams directly to the file; text writes exact bytes; structured values write a hirari.values/1 envelope."},
    {">>","VALUE >> DESTINATION\nAppend instead of replacing file contents. A callable sink receives -append before the value."},
    {"if","if CONDITION CONSEQUENT [ALTERNATE]\nOnly the chosen branch evaluates. Process success uses exit status."},
    {"for","for -ITEM -(VALUES ...) BODY\nIterate structured values. The previous ITEM binding is restored, including on failure."},
    {"while","while CONDITION BODY\nRepeat BODY while CONDITION holds, deciding truth exactly as if does (including program exit status). Each iteration is a separate reduction, so repetition never deepens the native stack. Ctrl+C interrupts; exit unwinds."},
    {"sleep","sleep SECONDS\nWait 0..86400 seconds (integer or real) without blocking interruption."},
    {"fail","fail MESSAGE ...\nRaise an operation failure with this message. Scripts stop with FILE:LINE unless the failure is observed with attempt."},
    {"attempt","attempt EXPRESSION\nEvaluate EXPRESSION and return a record: status \"ok\", \"failed\" (a program exited non-zero; exit_status and stderr as error) or \"error\" (a failure was raised; error is its message), plus value. False values are ok, not failures. Interruption and exit are never absorbed."},
    {"truth","truth VALUE\nTests Unit/false/zero/empty values, or a process's exit status."},
    {"source","source \"FILE.seed\" [ARGUMENTS ...]\nEvaluate definitions in the caller's scope. File-relative includes preserve argument frames."},
    {"run","run \"FILE.seed\" [ARGUMENTS ...]\nInvoke a Seed program. Restore caller definitions, cwd and world selection afterward. External effects remain."},
    {"arg","arg INDEX\nRead a zero-based structured program argument."},
    {"args","args\nReturn a reified product of all current program arguments."},
    {"argc","argc\nNumber of arguments in the active Seed program."},
    {"script-directory","script-directory\nDirectory of the currently executing .seed program."},
    {"path","path \"ROOT\" \"SEGMENT\" ...\nJoin filesystem paths without rewriting arbitrary program arguments."},
    {"apply","apply -PROGRAM -(ARGUMENTS ...)\nInvoke a reified program with a structured argument product. apply -(exec \"program\") (args) forwards script arguments."},
    {"exec","exec \"PROGRAM PATH\" [ARGUMENTS ...]\nLaunch an executable with argument values. No shell-text reparsing."},
    {"provide","provide -CAPABILITY [ARGUMENTS ...] | provide \"EXECUTABLE\" [ARGUMENTS ...]\nInvoke a linked library or external program through the same typed argument contract. Provider results remain inert data; live CLR identities cannot cross process boundaries."},
    {"capability","capability list | register -IDENTITY \"EXECUTABLE\" [\"DESCRIPTION\"]\nDiscover linked/process providers or register an external provider identity. Registration and help do not execute the provider."},
    {"data","data bind -NAME VALUE | get RECORD \"FIELD\" | at LIST INDEX | size VALUE | kind VALUE | form VALUE | read \"FILE\" | integer ANSWER | real ANSWER | json VALUE | text VALUE | print VALUES...\ndata record KEY VALUE ... | env \"NAME\" [VALUE] | file \"PATH\" | parse-json TEXT\nRetain and inspect typed data. form snapshots a value's structure and captured bindings for providers; abstraction bodies stay inert. integer/real explicitly decode native text; read restores one value envelope; kind reports physical representation. bind evaluates once; ordinary := retains a program form. get also reads a program answer's status, stdout and stderr. record builds a record from live values (keys are -names or text). env reads an environment variable (Unit when unset) or sets one for this shell and the programs it starts; () unsets. file reads a file's bytes as text. parse-json turns JSON text or program output into records, lists and values; null is Unit."},
    {"cd","cd [\"PATH\"]\nChange the filesystem working directory. Without an argument, use the user profile."},
    {"pwd","pwd\nShow the filesystem working directory."},
    {"exit","exit [STATUS]\nEnd this shell session with STATUS (default 0) and stop shell-owned jobs. Exit takes effect immediately, through loops, scripts and world entries. External services retain their own lifetimes."},
    {"quit","quit\nEnd Hirari. Equivalent to exit."},
    {"world","world open \"ROOT\" | world status\nRead a generic world.toml descriptor and invoke its Seed entry. Failures restore previous context."},
    {"vocab","vocab status | list | history -NAME\nvocab save -NAME [\"DESCRIPTION\"]\nvocab restore -NAME REVISION | forget -NAME\nvocab use \"DIRECTORY\"\nDurable structural definitions with revision history. Interactive top-level definitions save automatically; script scopes remain transient. Live CLR handles cannot be saved. HIRARI_STATE selects the user store; '-' disables it."}
};
}
seed::Ptr Shell::help(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto first = relation;
    bool prefix = r.is_symbol(first->head) &&
        (r.as_symbol(first->head)->text == "help" || r.as_symbol(first->head)->text == "?");
    auto second = static_cast<seed::Node*>(first->ref);
    auto& shell = *static_cast<Shell*>(context);
    auto finish = [&](std::string text) {
        auto value = r.arena().make_literal(text); shell.descriptions_.insert(value);
        return static_cast<seed::Ptr>(value);
    };
    if (!second && prefix) {
        std::ostringstream out; out << "Hirari: programs, values and live capabilities\n";
        for (const auto& [name, text] : documentation) out << name << "  ";
        for (const auto& [symbol, text] : r.registry().descriptions()) out << symbol->text << "  ";
        out << "\nUse help -COMMAND. Use inspect -COMMAND for deeper structure.\n";
        return finish(out.str());
    }
    if (!second || second->ref)
        throw std::runtime_error("help expects one identity; use help -NAME or inspect -NAME");
    seed::Ptr subject = prefix ? second->head : first->head;
    if (r.is_name(subject)) subject = r.as_name(subject)->target;
    // A syntax form documents the Program that owns its reserved head.
    if (r.pipeline_body(subject)) subject = r.intern("{}");
    if (r.is_cell(subject)) subject = r.as_cell(subject)->symbol;
    if (!r.is_symbol(subject))
        throw std::runtime_error("help expects an identity, not an expression; use inspect for structure");
    auto symbol = r.as_symbol(subject);
    std::ostringstream out; out << symbol->text << '\n';
    std::unordered_set<seed::Symbol*> seen;
    for (unsigned depth = 0; depth < 64; ++depth) {
        if (!seen.insert(symbol).second) { out << "cyclic identity alias\n"; break; }
        auto known = documentation.find(symbol->text);
        if (known != documentation.end()) { out << known->second << '\n'; break; }
        if (auto described = r.registry().description(symbol); !described.empty()) { out << described << '\n'; break; }
        if(auto description=shell.values_.description(symbol->text);!description.empty()){out<<description<<'\n';break;}
        if (auto cell = r.find_cell(symbol); cell && cell->metadata) {
            const auto& metadata = *cell->metadata;
            if (!metadata.docstring.empty()) out << metadata.docstring << '\n';
            if (metadata.kind == "abstraction") {
                out << "arguments:";
                for (const auto& parameter : metadata.parameters) out << ' ' << parameter;
                out << '\n';
            }
            out << "binding revision " << cell->revision << ": " << symbol->text
                << " := <" << metadata.kind << ">\n";
            out << "origin: " << metadata.origin << '\n';
            if (!metadata.source_file.empty()) out << "source: " << metadata.source_file << '\n';
            if (!metadata.provider.empty()) out << "consumer identity: " << metadata.provider << '\n';
            if (shell.vocabulary_) {
                auto annotation = shell.vocabulary_->annotation(symbol->text);
                if (!annotation.empty()) out << annotation << '\n';
            }
            if (!metadata.alias.empty()) {
                out << "alias of: " << metadata.alias << '\n';
                symbol = r.intern(metadata.alias); continue;
            }
            out << "Use inspect -" << symbol->text << " for executable structure.\n";
            break;
        }
        auto executable = ProcessHost::executable_path(symbol->text);
        if(!Shell::prefer_script(symbol->text) && !executable.empty()) {
            out << "native executable: " << executable << "\nNative --help is invoked explicitly.\n";
            break;
        }
        auto path = shell.find_script(symbol->text);
        if (!path.empty()) {
            const auto source = read_source(path);
            out << "Seed program: " << path_text(path) << '\n';
            const auto doc = r.source_documentation(source);
            if (!doc.empty()) out << doc << '\n';
            else {
                std::istringstream lines(source); std::string line;
                for (unsigned count = 0; count < 40 && std::getline(lines, line); ++count) {
                    auto begin = line.find_first_not_of(" \t\r");
                    if (begin == std::string::npos) continue;
                    if (line[begin] != '#') break;
                    if (line.compare(begin, 2, "#!") != 0) out << line.substr(begin) << '\n';
                }
            }
            break;
        }
        if (!executable.empty()) {
            out << "native executable: " << executable << "\nNative --help is invoked explicitly.\n";
            break;
        }
        out << "unresolved identity; define it or place a program on HIRARI_PATH/PATH\n";
        break;
    }
    return finish(out.str());
}

seed::Ptr Shell::inspect(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto terms=r.terms(relation);if(terms.empty())return relation;
    bool prefix=r.is_symbol(terms.front()) && r.as_symbol(terms.front())->text=="inspect";
    bool suffix=terms.size()>1 && r.is_symbol(terms.back()) && r.as_symbol(terms.back())->text=="inspect";
    if(!prefix && !suffix)return relation;
    auto& shell=*static_cast<Shell*>(context);
    auto finish=[&](std::string text){auto value=r.arena().make_literal(text);shell.descriptions_.insert(value);return static_cast<seed::Ptr>(value);};
    if(terms.size()==1){std::ostringstream out;out<<"Hirari: programs, values and live capabilities\n";for(const auto& [name,text]:documentation)out<<name<<"  ";for(const auto& [symbol,text]:r.registry().descriptions())out<<symbol->text<<"  ";out<<"\nUse ? COMMAND or COMMAND help. Native --help is invoked explicitly.\n";return finish(out.str());}
    std::vector<seed::Ptr> subjects(prefix?terms.begin()+1:terms.begin(),prefix?terms.end():terms.end()-1);
    auto subject=subjects.size()==1?subjects.front():r.product(subjects);
    std::ostringstream out;out<<r.print(subject)<<'\n';
    std::unordered_set<seed::Ptr> seen;
    for(unsigned depth=0;depth<32;++depth) {
        if(!seen.insert(subject).second){out<<"cyclic definition\n";break;}
        if(shell.values_.opaque(subject)){out<<shell.values_.describe_foreign(subject);break;}
        if(r.is_cell(subject)){out<<"binding cell revision "<<r.as_cell(subject)->revision<<'\n';subject=r.as_cell(subject)->symbol;continue;}
        if(r.is_name(subject)){
            auto target=r.as_name(subject)->target;if(!r.is_symbol(target) && !r.pipeline_body(target)){out<<"reified value: "<<r.print(target)<<'\n';break;}subject=target;
        }
        if(r.is_abstraction(subject)){
            seed::Ptr binder=nullptr,body=nullptr;r.abstraction(subject,binder,body);
            auto first=r.is_relation(body)?r.terms(r.as_node(body)).front():body;
            if(r.is_literal(first))out<<r.as_literal(first)->bytes<<'\n';
            out<<"abstraction: "<<r.print(subject)<<"\nApply an argument to substitute its binder. Incoming symbols may be captured. The body is not evaluated until application.\n";break;
        }
        if(r.is_relation(subject)){out<<"retained program form: "<<r.print(subject)<<'\n';auto t=r.terms(r.as_node(subject));if(t.empty())break;subject=t.front();continue;}
        if(!r.is_symbol(subject)){out<<"value: "<<r.print(subject)<<'\n';break;}
        auto symbol=r.as_symbol(subject);auto found=documentation.find(symbol->text);
        if(found!=documentation.end()){out<<found->second<<'\n';break;}
        if(auto described=r.registry().description(symbol);!described.empty()){out<<described<<'\n';break;}
        if(auto cell=r.find_cell(symbol);cell && cell->head){
            out<<"binding revision "<<cell->revision<<": "<<symbol->text<<" := "<<r.print(cell->head)<<'\n';
            if(cell->metadata){out<<"origin: "<<cell->metadata->origin<<'\n';if(!cell->metadata->source_file.empty())out<<"source: "<<cell->metadata->source_file<<'\n';}
            if(shell.vocabulary_)out<<shell.vocabulary_->annotation(symbol->text)<<'\n';subject=cell->head;continue;
        }
        auto executable=ProcessHost::executable_path(symbol->text);
        if(!Shell::prefer_script(symbol->text) && !executable.empty()){out<<"native executable: "<<executable<<"\nUse exec \""<<executable<<"\" \"--help\" for its own documentation.\n";break;}
        auto path=shell.find_script(symbol->text);
        if(!path.empty()){
            auto source=read_source(path);auto form=r.read(source);
            auto first=r.is_relation(form)?r.terms(r.as_node(form)).front():form;
            out<<"Seed program: "<<path_text(path)<<'\n';
            if(r.is_literal(first))out<<r.as_literal(first)->bytes<<'\n';
            std::istringstream lines(source);std::string line;unsigned count=0;
            while(count++<40 && std::getline(lines,line)){auto comment=line.find_first_not_of(" \t\r");if(comment==std::string::npos)continue;if(line[comment]!='#')break;out<<line.substr(comment)<<'\n';}break;
        }
        if(!executable.empty()){out<<"native executable: "<<executable<<"\nUse exec \""<<executable<<"\" \"--help\" for its own documentation.\n";break;}
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
        store.save(r,identity(r,terms[2]),description,[&](auto p){return shell.values_.opaque(p);});return r.arena().unit();
    }
    if(op=="forget" && terms.size()==3){store.forget(r,identity(r,terms[2]));return r.arena().unit();}
    if(op=="restore" && terms.size()==4){auto s=r.print(r.reduce(terms[3]));if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("revision requires a positive integer");store.restore(r,identity(r,terms[2]),std::stoull(s));return r.arena().unit();}
    throw std::runtime_error("unknown vocabulary operation; use vocab help");
}
}
