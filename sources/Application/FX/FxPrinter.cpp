#include "FxPrinter.h"
#include "Services/Audio/Audio.h"

FxPrinter::FxPrinter(ViewData* viewData)
    : samples_dir("project:samples"),
      impulse_dir("root:samplelib"),
      viewData_(viewData) {
    int curInstr = viewData_->currentInstrument_;
    InstrumentBank* bank = viewData_->project_->GetInstrumentBank();
    instrument_ = static_cast<SampleInstrument *>(bank->GetInstrument(curInstr));
    notificationResult_ = "";
    // Prefer a bundled binary alongside the executable over the system one.
    // Check in order: bin:ffmpig → bin:ffmpeg → system "ffmpeg"
    ffmpeg_ = "ffmpeg";
    const char *candidates[] = { "bin:ffmpig", "bin:ffmpeg", nullptr };
    for (int i = 0; candidates[i]; i++) {
        Path p(candidates[i]);
        if (Path(p.GetPath().c_str()).Exists()) {
            ffmpeg_ = p.GetPath();
            break;
        }
    }
}

void FxPrinter::setParams() {
    irPad_ = instrument_->FindVariable(SIP_IR_PAD)->GetInt();
    irWet_ = instrument_->FindVariable(SIP_IR_WET)->GetInt();
}

void FxPrinter::setPaths() {
    fi_ = std::string(instrument_->GetName());

    foWav_ = fi_.substr(0, fi_.find_last_of('.')) + "_.wav";
    fo_ = "\"" + samples_dir.GetPath() + '/' + foWav_ + "\"";

    ir_ = impulse_dir.GetPath() + "/IR-s/";
    ir_ += std::string(instrument_->FindVariable(SIP_PRINTFX)->GetString());
}

std::string FxPrinter::parseCommand() {
    std::string command;
    float padDur = static_cast<float>(irPad_) / 1000;
    float smplLength = static_cast<float>(instrument_->GetSampleSize()) / Audio::GetInstance()->GetSampleRate();

    std::ostringstream cm1, cm2, cm3, cm4, cm5;
    cm1 << ffmpeg_ << " -y -i "
        << "\"" << samples_dir.GetPath() << "/" << fi_ << "\""
        << " -i " << ir_ << " -filter_complex ";
    // bug in ffmpeg version 4.4.2
    // requires pad_dur to be over 0 or output will be infinitely long
    if (irPad_ > 0) {
        cm2 << "\"[0:a]apad=pad_dur=" << padDur << "[pad];[pad]";
    } else {
        cm2 << "\"[0:a]";
    }
    cm3 << "[1:a]afir=dry=10:wet=10[reverb];"
        << "[0] [reverb] amix=inputs=2:weights=100 " << irWet_ << ",volume=1.5";
    if (irPad_ > 0) {
        cm4 << ",afade=out:st=" << smplLength << ":d=" << padDur;
    } else {
        cm4 << "";
    }
    cm5 << "\" " << fo_.c_str();
    command = cm1.str() + cm2.str() + cm3.str() + cm4.str() + cm5.str();
    return command;
}

char *FxPrinter::GetNotification() { return notificationResult_; }

bool FxPrinter::Run() {
    setParams();
    setPaths();
    // Are we overwriting an already imported sample?
    bool imported = SamplePool::GetInstance()->IsImported(foWav_);
    std::string cmd = parseCommand();
    
    if (system(cmd.c_str()) == 0) {
        int newIndex = SamplePool::GetInstance()->Reassign(foWav_, imported);
        instrument_->AssignSample(newIndex);
        notificationResult_ = "OK!";
        return true;
    } else {
        
        notificationResult_ = "Failed, check lgpt.log";
        return false;
    }
}
