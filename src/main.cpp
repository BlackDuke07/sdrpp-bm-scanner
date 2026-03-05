#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <module.h>
#include <core.h>
#include <config.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <signal_path/vfo_manager.h>

SDRPP_MOD_INFO{
    /* Name:            */ "scannerpp",
    /* Description:     */ "Bookmark scanner module for SDR++",
    /* Author:          */ "BlackDuke07",
    /* Version:         */ 0, 1, 3,
    /* Max instances    */ 1
};

namespace sigpath {
    SDRPP_EXPORT VFOManager vfoManager;
}

namespace {
    constexpr auto FIXED_DWELL = std::chrono::milliseconds(100);
    constexpr auto BOOKMARK_RELOAD_INTERVAL = std::chrono::seconds(1);
    constexpr float TOP_OVERLAY_OFFSET_X = 748.0f;
    constexpr float TOP_OVERLAY_OFFSET_Y = 9.0f;
    constexpr float TOP_OVERLAY_FONT_SCALE = 2.45f;

    struct Bookmark {
        std::string name;
        double frequency = 0.0;
        double bandwidth = 0.0;
    };

    ConfigManager moduleConfig;

    class ScannerPPModule : public ModuleManager::Instance {
    public:
        explicit ScannerPPModule(std::string instanceName)
            : name_(std::move(instanceName)) {
            moduleConfig.acquire();
            snrThresholdDb_ = moduleConfig.conf.contains("snrThresholdDb")
                ? std::clamp(static_cast<int>(std::lround(static_cast<double>(moduleConfig.conf["snrThresholdDb"]))), 0, 30)
                : 8;
            resumeDelaySec_ = moduleConfig.conf.contains("resumeDelaySec")
                ? std::clamp(static_cast<int>(moduleConfig.conf["resumeDelaySec"]), 0, 5)
                : 1;
            onlyVisible_ = moduleConfig.conf.contains("onlyVisible")
                ? static_cast<bool>(moduleConfig.conf["onlyVisible"])
                : false;
            showTopOverlay_ = moduleConfig.conf.contains("showTopOverlay")
                ? static_cast<bool>(moduleConfig.conf["showTopOverlay"])
                : true;
            moduleConfig.release();

            nextStepAt_ = std::chrono::steady_clock::now();
            lastBookmarksReload_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);

            gui::menu.registerEntry(name_, drawMenu, this, NULL);
        }

        ~ScannerPPModule() override {
            stopScan(true);
            gui::menu.removeEntry(name_);
        }

        void postInit() override {}

        void enable() override {
            enabled_ = true;
        }

        void disable() override {
            enabled_ = false;
            stopScan(false);
        }

        bool isEnabled() override {
            return enabled_;
        }

    private:
        static void drawMenu(void* ctx) {
            auto* self = static_cast<ScannerPPModule*>(ctx);
            self->draw();
        }

        void draw() {
            if (!enabled_) {
                return;
            }

            maybeReloadBookmarks();
            buildScanList(scanCandidates_);
            updateScannerState(scanCandidates_);
            drawTopOverlay();
            const int visibleBookmarks = countVisibleBookmarks();

            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float totalWidth = ImGui::GetContentRegionAvail().x;
            const float btnWidth = std::max(10.0f, (totalWidth - (3.0f * gap)) / 4.0f);

            if (!scanning_) {
                if (ImGui::Button("Scan", ImVec2(btnWidth, 0))) {
                    startScan(scanCandidates_);
                }
            }
            else {
                if (ImGui::Button("Stop", ImVec2(btnWidth, 0))) {
                    stopScan(false);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Next", ImVec2(btnWidth, 0))) {
                stepToNextBookmark(scanCandidates_);
            }
            ImGui::SameLine();
            if (ImGui::Button(hold_ ? "Unhold" : "Hold", ImVec2(btnWidth, 0))) {
                hold_ = !hold_;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload", ImVec2(btnWidth, 0))) {
                reloadBookmarks();
            }

            ImGui::Separator();

            ImGui::Text("Current: %s", currentBookmarkName_.empty() ? "(none)" : currentBookmarkName_.c_str());
            ImGui::Text("State: %s", currentStateText().c_str());

            ImGui::Separator();
            if (ImGui::SliderInt("Lock SNR (dB)", &snrThresholdDb_, 0, 30)) {
                persistSettings();
            }
            if (ImGui::SliderInt("Resume Delay (s)", &resumeDelaySec_, 0, 5)) {
                persistSettings();
            }

            if (ImGui::Checkbox("Only Scan Visible Bookmarks", &onlyVisible_)) {
                persistSettings();
                scanIndex_ = 0;
            }
            if (ImGui::Checkbox("Show Top Name Overlay", &showTopOverlay_)) {
                persistSettings();
            }

            ImGui::Separator();
            ImGui::Text("Active list: %s", activeListName_.empty() ? "(none)" : activeListName_.c_str());
            ImGui::Text("Bookmarks: %d total, %d visible", static_cast<int>(allBookmarks_.size()), visibleBookmarks);
        }

        std::string currentStateText() const {
            if (hold_) {
                return "Hold";
            }
            if (locked_) {
                return "Locked";
            }
            if (scanning_) {
                if (resumeAfterUnlockAt_ != std::chrono::steady_clock::time_point::min() &&
                    std::chrono::steady_clock::now() < resumeAfterUnlockAt_) {
                    return "Waiting";
                }
                return "Scanning";
            }
            return "Stopped";
        }

        void startScan(const std::vector<const Bookmark*>& candidates) {
            if (!gui::mainWindow.isPlaying()) {
                scanning_ = false;
                locked_ = false;
                return;
            }

            if (candidates.empty()) {
                scanning_ = false;
                locked_ = false;
                return;
            }

            scanning_ = true;
            locked_ = false;
            scanIndex_ = 0;
            nextStepAt_ = std::chrono::steady_clock::now();
            resumeAfterUnlockAt_ = std::chrono::steady_clock::time_point::min();
        }

        void stopScan(bool clearCurrent) {
            scanning_ = false;
            locked_ = false;
            hold_ = false;
            resumeAfterUnlockAt_ = std::chrono::steady_clock::time_point::min();
            if (clearCurrent) {
                currentBookmarkName_.clear();
            }
        }

        void updateScannerState(const std::vector<const Bookmark*>& candidates) {
            if (!scanning_) {
                return;
            }

            if (!gui::mainWindow.isPlaying()) {
                stopScan(false);
                return;
            }

            if (candidates.empty()) {
                stopScan(false);
                return;
            }

            const float currentSnr = gui::waterfall.selectedVFOSNR;
            const auto now = std::chrono::steady_clock::now();

            if (hold_) {
                return;
            }

            if (locked_) {
                if (currentSnr < (static_cast<float>(snrThresholdDb_) - 1.5f)) {
                    locked_ = false;
                    resumeAfterUnlockAt_ = now + std::chrono::seconds(resumeDelaySec_);
                    nextStepAt_ = resumeAfterUnlockAt_;
                }
                return;
            }

            if (!currentBookmarkName_.empty() && currentSnr >= static_cast<float>(snrThresholdDb_)) {
                locked_ = true;
                return;
            }

            if (now < resumeAfterUnlockAt_) {
                return;
            }

            if (now < nextStepAt_) {
                return;
            }

            if (scanIndex_ >= candidates.size()) {
                scanIndex_ = 0;
            }

            const Bookmark& target = *candidates[scanIndex_];
            tuneToBookmark(target);
            currentBookmarkName_ = target.name;

            scanIndex_ = (scanIndex_ + 1) % candidates.size();
            nextStepAt_ = now + FIXED_DWELL;

            if (gui::waterfall.selectedVFOSNR >= static_cast<float>(snrThresholdDb_)) {
                locked_ = true;
            }
        }

        void stepToNextBookmark(const std::vector<const Bookmark*>& candidates) {
            if (candidates.empty()) {
                return;
            }

            if (scanIndex_ >= candidates.size()) {
                scanIndex_ = 0;
            }

            const Bookmark& target = *candidates[scanIndex_];
            tuneToBookmark(target);
            currentBookmarkName_ = target.name;
            locked_ = false;
            scanIndex_ = (scanIndex_ + 1) % candidates.size();
            nextStepAt_ = std::chrono::steady_clock::now() + FIXED_DWELL;
            resumeAfterUnlockAt_ = std::chrono::steady_clock::time_point::min();
        }

        void tuneToBookmark(const Bookmark& bm) {
            const std::string vfoName = getTargetVFO();

            if (vfoName.empty()) {
                gui::waterfall.setCenterFrequency(bm.frequency);
                gui::waterfall.centerFreqMoved = true;
                return;
            }

            if (bm.bandwidth > 1.0 && sigpath::vfoManager.vfoExists(vfoName)) {
                sigpath::vfoManager.setBandwidth(vfoName, bm.bandwidth);
            }

            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
        }

        void drawTopOverlay() const {
            if (!showTopOverlay_ || !scanning_ || currentBookmarkName_.empty()) {
                return;
            }

            ImGuiViewport* viewport = ImGui::GetMainViewport();
            if (viewport == nullptr) {
                return;
            }

            const ImVec2 pos(viewport->Pos.x + TOP_OVERLAY_OFFSET_X, viewport->Pos.y + TOP_OVERLAY_OFFSET_Y);
            ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
            if (drawList == nullptr) {
                return;
            }

            ImFont* font = ImGui::GetFont();
            const float fontSize = ImGui::GetFontSize() * TOP_OVERLAY_FONT_SCALE;
            drawList->AddText(font, fontSize, pos, IM_COL32(255, 255, 255, 255), currentBookmarkName_.c_str());
        }

        std::string getTargetVFO() const {
            if (!gui::waterfall.selectedVFO.empty()) {
                return gui::waterfall.selectedVFO;
            }
            if (sigpath::vfoManager.vfoExists("Radio")) {
                return "Radio";
            }
            return "";
        }

        void buildScanList(std::vector<const Bookmark*>& out) const {
            out.clear();
            out.reserve(allBookmarks_.size());

            if (!onlyVisible_) {
                for (const auto& bm : allBookmarks_) {
                    out.push_back(&bm);
                }
                return;
            }

            const double centerHz = gui::waterfall.getCenterFrequency() + gui::waterfall.getViewOffset();
            const double spanHz = gui::waterfall.getViewBandwidth();
            const double lowHz = centerHz - (spanHz * 0.5);
            const double highHz = centerHz + (spanHz * 0.5);

            for (const auto& bm : allBookmarks_) {
                if (bm.frequency >= lowHz && bm.frequency <= highHz) {
                    out.push_back(&bm);
                }
            }
        }

        int countVisibleBookmarks() const {
            const double centerHz = gui::waterfall.getCenterFrequency() + gui::waterfall.getViewOffset();
            const double spanHz = gui::waterfall.getViewBandwidth();
            const double lowHz = centerHz - (spanHz * 0.5);
            const double highHz = centerHz + (spanHz * 0.5);

            int count = 0;
            for (const auto& bm : allBookmarks_) {
                if (bm.frequency >= lowHz && bm.frequency <= highHz) {
                    ++count;
                }
            }
            return count;
        }

        void maybeReloadBookmarks() {
            const auto now = std::chrono::steady_clock::now();
            if ((now - lastBookmarksReload_) > BOOKMARK_RELOAD_INTERVAL) {
                reloadBookmarks();
            }
        }

        void reloadBookmarks() {
            lastBookmarksReload_ = std::chrono::steady_clock::now();

            std::ifstream fs(freqManagerConfigPath(), std::ios::in);
            if (!fs.is_open()) {
                activeListName_.clear();
                allBookmarks_.clear();
                return;
            }

            json doc;
            try {
                fs >> doc;
            }
            catch (...) {
                activeListName_.clear();
                allBookmarks_.clear();
                return;
            }

            if (!doc.contains("selectedList") || !doc.contains("lists") || !doc["lists"].is_object()) {
                activeListName_.clear();
                allBookmarks_.clear();
                return;
            }

            activeListName_ = static_cast<std::string>(doc["selectedList"]);
            if (!doc["lists"].contains(activeListName_)) {
                allBookmarks_.clear();
                return;
            }

            const auto& active = doc["lists"][activeListName_];
            if (!active.contains("bookmarks") || !active["bookmarks"].is_object()) {
                allBookmarks_.clear();
                return;
            }

            std::vector<Bookmark> next;
            next.reserve(active["bookmarks"].size());
            for (const auto& [name, value] : active["bookmarks"].items()) {
                if (!value.contains("frequency") || !value.contains("bandwidth")) {
                    continue;
                }
                Bookmark bm;
                bm.name = name;
                bm.frequency = static_cast<double>(value["frequency"]);
                bm.bandwidth = static_cast<double>(value["bandwidth"]);
                next.push_back(bm);
            }
            std::sort(next.begin(), next.end(), [](const Bookmark& a, const Bookmark& b) {
                if (a.frequency == b.frequency) {
                    return a.name < b.name;
                }
                return a.frequency < b.frequency;
            });
            allBookmarks_ = std::move(next);

            if (!currentBookmarkName_.empty()) {
                const bool exists = std::any_of(allBookmarks_.begin(), allBookmarks_.end(), [this](const Bookmark& bm) {
                    return bm.name == currentBookmarkName_;
                });
                if (!exists) {
                    currentBookmarkName_.clear();
                    locked_ = false;
                }
            }
        }

        std::string freqManagerConfigPath() const {
            return core::args["root"].s() + "/frequency_manager_config.json";
        }

        void persistSettings() {
            moduleConfig.acquire();
            moduleConfig.conf["snrThresholdDb"] = snrThresholdDb_;
            moduleConfig.conf["resumeDelaySec"] = resumeDelaySec_;
            moduleConfig.conf["onlyVisible"] = onlyVisible_;
            moduleConfig.conf["showTopOverlay"] = showTopOverlay_;
            moduleConfig.release(true);
        }

        std::string name_;
        bool enabled_ = true;
        bool scanning_ = false;
        bool locked_ = false;
        bool hold_ = false;
        bool onlyVisible_ = false;
        bool showTopOverlay_ = true;
        int snrThresholdDb_ = 8;
        int resumeDelaySec_ = 1;
        size_t scanIndex_ = 0;
        std::string activeListName_;
        std::string currentBookmarkName_;
        std::vector<Bookmark> allBookmarks_;
        std::vector<const Bookmark*> scanCandidates_;
        std::chrono::steady_clock::time_point nextStepAt_;
        std::chrono::steady_clock::time_point resumeAfterUnlockAt_ = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point lastBookmarksReload_;
    };
}

MOD_EXPORT void _INIT_() {
    json def = json::object();
    def["snrThresholdDb"] = 8.0;
    def["resumeDelaySec"] = 1;
    def["onlyVisible"] = false;
    def["showTopOverlay"] = true;

    moduleConfig.setPath(core::args["root"].s() + "/scannerpp_config.json");
    moduleConfig.load(def);
    moduleConfig.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ScannerPPModule(std::move(name));
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ScannerPPModule*)instance;
}

MOD_EXPORT void _END_() {
    moduleConfig.disableAutoSave();
    moduleConfig.save();
}
