#include "headless.h"
#include "cpu/cpu.h"
#include "peripheral/mcp230xx.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <unistd.h>

static void Require(bool ok, const char* message) { if (!ok) { std::fprintf(stderr,"headless: %s\n",message); std::exit(1); } }
struct Pipe {
    int fd[2]; Pipe() { Require(pipe(fd)==0,"pipe"); }
    ~Pipe() { close(fd[0]); if(fd[1]>=0)close(fd[1]); }
    void Send(const std::string& text) { Require(write(fd[1],text.data(),text.size())==static_cast<ssize_t>(text.size()),"pipe write"); }
    void End() { close(fd[1]); fd[1]=-1; }
};
class InputCpu : public BlackFinCpu {
public:
    void Drain() { ProcessEvents();ProcessEvents(); }
    GPIOPinLevel Pin() { return gpioExpanders.at(5)->GetPinOutput(2); }
};
int main(int argc,char** argv) {
    Require(argc==2,"config argument");
    const std::string config=std::filesystem::absolute(argv[1]).string();
    using Clock=HeadlessFrontend::Clock;using namespace std::chrono_literals;
    const auto start=Clock::now();
    {
        Pipe pipe;HeadlessFrontend frontend(config,pipe.fd[0]);
        std::vector<std::tuple<int,int,bool>> keys;unsigned frames=0;
        frontend.SetKeyEventCallback([&](int bank,int pin,bool press){keys.emplace_back(bank,pin,press);});
        frontend.SetOnFrameStartCallback([&](Display&){++frames;});
        pipe.Send("press shift\nwait 50\ntap f3# 100\nrelease shift\nquit");pipe.End();
        frontend.PollEvents(start);Require(keys==std::vector<std::tuple<int,int,bool>>{{5,6,true}},"key wiring and wait");
        frontend.PollEvents(start+49ms);Require(keys.size()==1 && frames==2,"wait does not block frame callback");
        frontend.PollEvents(start+50ms);Require(keys.back()==std::make_tuple(3,13,true),"sharp key parsed");
        frontend.PollEvents(start+149ms);Require(keys.size()==2,"tap held");
        frontend.PollEvents(start+150ms);Require(keys.back()==std::make_tuple(3,13,false),"tap released");
        frontend.PollEvents(start+166ms);Require(frontend.ShouldClose() && keys.back()==std::make_tuple(5,6,false),"final line without newline");
    }
    for(const auto& bad:{"press missing","tap play 0","wait -1","volume 256","accel 32768 0 0","wait-frames 0","screenshot /tmp/no-frame","unknown"}) {
        Pipe pipe;HeadlessFrontend frontend(config,pipe.fd[0]);pipe.Send(std::string(bad)+"\n");
        bool rejected=false;try{frontend.PollEvents(start);}catch(const std::exception&){rejected=true;}
        Require(rejected,"malformed commands rejected");
    }
    {
        Pipe pipe;HeadlessFrontend frontend(config,pipe.fd[0]);unsigned keys=0;
        frontend.SetKeyEventCallback([&](int,int,bool){++keys;});pipe.Send("press pl");frontend.PollEvents(start);
        Require(keys==0,"partial line is nonblocking");pipe.Send("ay\n");frontend.PollEvents(start+16ms);Require(keys==1,"partial line completes");
        pipe.End();frontend.PollEvents(start+32ms);Require(!frontend.ShouldClose(),"EOF is not quit");
    }
    {
        Pipe pipe;HeadlessFrontend frontend(config,pipe.fd[0]);pipe.Send("accel 64 -64 512\nvolume 200\n");frontend.PollEvents(start);
        Require(frontend.GetSensors().accelFixed && frontend.GetSensors().ay==-64 && frontend.GetSensors().volume==200,"scripted sensors");
        frontend.Initialize(2,2);
        const std::array<uint8_t,4> first{0x00,0xF8,0xE0,0x07},second{0x1F,0,0xFF,0xFF};
        frontend.UpdateRowBuffer(0,1,second.data(),4);Require(frontend.FrameCount()==0,"tail without origin ignored");
        frontend.UpdateRowBuffer(0,0,first.data(),4);frontend.Initialize(2,2);frontend.UpdateRowBuffer(0,1,second.data(),4);
        Require(frontend.FrameCount()==1,"same geometry preserves partial frame");
        const auto output=std::filesystem::temp_directory_path()/("op1-headless-"+std::to_string(getpid())+".ppm");
        frontend.WriteScreenshot(output.string());
        std::ifstream file(output,std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(file),{}};
        const std::string header="P6\n2 2\n255\n";
        const std::array<uint8_t,12> rgb{0,0,255,255,0,0,255,255,255,0,255,0};
        Require(bytes.size()==header.size()+rgb.size() && bytes.substr(0,header.size())==header,"PPM header");
        for(size_t i=0;i<rgb.size();++i)Require(static_cast<uint8_t>(bytes[header.size()+i])==rgb[i],"rotation and full-scale RGB565 colors");
        bool rejected=false;try{frontend.WriteScreenshot(output.string());}catch(const std::exception&){rejected=true;}
        Require(rejected,"screenshots do not overwrite existing files");std::filesystem::remove(output);
        pipe.Send("wait-frames 1 100\nvolume 7\n");frontend.PollEvents(start+16ms);
        frontend.PollEvents(start+115ms);Require(frontend.GetSensors().volume==200,"old frames do not satisfy wait");
        frontend.UpdateRowBuffer(0,0,first.data(),4);frontend.UpdateRowBuffer(0,1,second.data(),4);
        frontend.PollEvents(start+116ms);Require(frontend.GetSensors().volume==7,"new complete frame resumes commands");
        pipe.Send("wait-frames 1 100\n");frontend.PollEvents(start+132ms);
        rejected=false;try{frontend.PollEvents(start+232ms);}catch(const std::exception&){rejected=true;}Require(rejected,"frame wait timeout fails fast");
    }
    {
        Pipe pipe;auto frontend=std::make_shared<HeadlessFrontend>(config,pipe.fd[0]);InputCpu cpu;cpu.AttachKeyboard(frontend);
        pipe.Send("release play\n");frontend->PollEvents(start);cpu.Drain();Require(cpu.Pin()==GPIOPinLevel::High,"queued release reaches expander");
        pipe.Send("press play\n");frontend->PollEvents(start+16ms);Require(cpu.Pin()==GPIOPinLevel::High,"frontend does not mutate guest immediately");
        cpu.Drain();Require(cpu.Pin()==GPIOPinLevel::Low,"queued logical key reaches production platform");
    }
    std::puts("headless: input, observation, screenshot, timeout, and platform routing passed");
}
