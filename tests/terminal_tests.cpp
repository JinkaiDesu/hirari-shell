// The prompt, as a person meets it: hirari on a real pseudoterminal, driven by keystrokes.
// A terminal re-renders the screen, so typed Seed reports each result through a file.
#include "hirari/terminal.hpp"
#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
namespace {
using namespace std::chrono_literals;
const std::string left="\x1b[D",up="\x1b[A",home="\x1b[H",end="\x1b[F",backspace="\x7f",tab="\t",enter="\r";
const std::string ctrl_a="\x01",ctrl_e="\x05",ctrl_k="\x0b",ctrl_u="\x15",ctrl_w="\x17",ctrl_y="\x19",ctrl_r="\x12",ctrl_z="\x1a",ctrl_d="\x04";
std::string repeat(const std::string& keys,std::size_t count){std::string out;for(std::size_t i=0;i<count;++i)out+=keys;return out;}
std::string read(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const std::filesystem::path& path,std::string_view text){std::ofstream(path,std::ios::binary)<<text;}

class Screen final:public hirari::Host {
public:
    void write(std::string_view bytes) override {std::lock_guard lock(mutex_);text_+=bytes;}
    void error(std::string_view bytes) override {write(bytes);}
    std::string tail() {std::lock_guard lock(mutex_);return text_.size()>2000?text_.substr(text_.size()-2000):text_;}
private:
    std::mutex mutex_;
    std::string text_;
};

class Terminal {
public:
    explicit Terminal(const std::string& executable):pty_(screen_) {
        pty_.start(executable,{},120,30,[this](int code){status_.store(code);});
    }
    void type(const std::string& keys) {pty_.write(keys);}
    // Tab acts only when no more keys are waiting behind it.
    void complete(const std::string& keys) {type(keys+tab);std::this_thread::sleep_for(300ms);}
    void expect(const std::filesystem::path& path,const std::string& expected) {
        auto deadline=std::chrono::steady_clock::now()+30s;
        for(;;) {
            auto actual=read(path);
            if(actual==expected)return;
            if(std::chrono::steady_clock::now()>deadline)
                throw std::runtime_error(path.filename().string()+": expected ["+expected+"], found ["+actual+"]\nScreen:\n"+screen_.tail());
            std::this_thread::sleep_for(20ms);
        }
    }
    int finish(const std::string& keys) {
        type(keys);
        auto deadline=std::chrono::steady_clock::now()+30s;
        while(pty_.running()) {
            if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("the shell did not end\nScreen:\n"+screen_.tail());
            std::this_thread::sleep_for(20ms);
        }
        return status_.load();
    }
private:
    Screen screen_;
    hirari::TerminalSession pty_;
    std::atomic_int status_{-1};
};

std::string statement(const std::string& text,const std::string& file) {return "{ \""+text+"\" > \""+file+"\" }";}
}

int run_tests(const std::vector<std::string>& argv) {
    auto before=std::filesystem::current_path();
    auto root=std::filesystem::canonical(std::filesystem::temp_directory_path())/("Hirari terminal "+std::to_string(hirari::platform::process_id()));
    try {
        if(argv.size()!=2)throw std::runtime_error("hirari path required");
        const std::string hirari=argv[1];
        std::filesystem::create_directories(root/"tabdir");
        std::filesystem::current_path(root);
        hirari::platform::set_environment("HIRARI_STATE",hirari::path_text(root/"state"));
        hirari::platform::set_environment("HIRARI_STARTUP",std::nullopt);
        write(root/"tabfile-unique.txt","old");
        write(root/"tabdir"/"inner.txt","old");

        {
            Terminal t(hirari);
            // Left inserts in the middle of the line, also among wide characters.
            auto line=statement("hlo","left.txt");
            t.type(line+repeat(left,line.size()-line.find("lo"))+"el"+enter);
            t.expect("left.txt","hello");
            t.type(statement("日","wide.txt")+repeat(left,std::string("\" > \"wide.txt\" }").size())+"本"+enter);
            t.expect("wide.txt","日本");
            // Home and Ctrl+A go to the start, End and Ctrl+E back to the end.
            auto rest=statement("home","home.txt").substr(1);
            t.type(rest+home+"{"+enter);
            t.expect("home.txt","home");
            write(root/"home.txt","");
            t.type(rest.substr(0,rest.size()-1)+ctrl_a+"{"+end+"}"+enter);
            t.expect("home.txt","home");
            write(root/"home.txt","");
            t.type(rest.substr(0,rest.size()-1)+home+"{"+ctrl_e+"}"+enter);
            t.expect("home.txt","home");
            // Backspace removes what is before the cursor.
            t.type("{ \"bacX"+backspace+"k\" > \"back.txt\" }"+enter);
            t.expect("back.txt","back");
            // Ctrl+U clears to the start, and Ctrl+Y brings it back.
            t.type("this is not Seed ("+ctrl_u+statement("cleared","clear.txt")+enter);
            t.expect("clear.txt","cleared");
            t.type(statement("yanked","yank.txt")+ctrl_u+ctrl_y+enter);
            t.expect("yank.txt","yanked");
            // Ctrl+K clears to the end.
            const std::string junk=" trailing junk (";
            t.type(statement("killed","kill.txt")+junk+repeat(left,junk.size())+ctrl_k+enter);
            t.expect("kill.txt","killed");
            // Ctrl+W takes one word back.
            t.type("{ \"word junk"+ctrl_w+"\" > \"word.txt\" }"+enter);
            t.expect("word.txt","word ");
            // A line longer than the terminal wraps and still edits as one line.
            const std::string longer(200,'a');
            t.type(statement(longer,"long.txt").substr(1)+home+"{"+enter);
            t.expect("long.txt",longer);
            // Tab finishes a unique file name, closing its string.
            t.complete("{ \"tab\" > \"tabfile-un");
            t.type("}"+enter);
            t.expect("tabfile-unique.txt","tab");
            // A directory stays open, so the next Tab can go inside it.
            t.complete("{ \"inside\" > \"tabd");
            t.complete("inn");
            t.type("}"+enter);
            t.expect(root/"tabdir"/"inner.txt","inside");
            // Up recalls the last line.
            t.type("{ \"first\" >> \"history.txt\" }"+enter);
            t.expect("history.txt","first");
            t.type(up+enter);
            t.expect("history.txt","firstfirst");
            // The end-of-input key on an empty line ends the shell without entering a line.
#ifdef _WIN32
            t.finish(ctrl_z);
#else
            t.finish(ctrl_d);
#endif
        }
        {
            // History survives the shell.
            Terminal t(hirari);
            t.type(up+enter);
            t.expect("history.txt","firstfirstfirst");
            // Ctrl+R finds an older line, and Enter runs it.
            t.type(statement("marker","marker.txt")+enter);
            t.expect("marker.txt","marker");
            t.type(ctrl_r+"\"first"+enter);
            t.expect("history.txt","firstfirstfirstfirst");
            if(t.finish("exit"+enter)!=0)throw std::runtime_error("exit did not end with status 0");
        }
        std::filesystem::current_path(before);
        std::filesystem::remove_all(root);
        std::cout<<"TERMINAL_OK\n";
        return 0;
    } catch(const std::exception& error) {
        std::filesystem::current_path(before);
        std::cerr<<error.what()<<"\nEvidence: "<<root.string()<<'\n';
        return 1;
    }
}
HIRARI_TEST_MAIN(run_tests)
