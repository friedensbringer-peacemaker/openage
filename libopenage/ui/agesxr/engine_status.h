// xr.ages — Zustand des Engine-Starts und Text des VR-Ladebildschirms (Plan 1c, Teilschritt 1c-6).
// Ohne openage- und Android-Abhängigkeit: Host-getestet (tests/test_engine_logic.cpp). EngineHost füllt den
// Zustand, die XR-Schicht zeichnet daraus den Ladebildschirm, bis das erste Bild der Engine da ist.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace agesxr {

enum class EnginePhase : int {
    kIdle = 0,
    kPreparing,  // Dateien aus der APK entpacken
    kChecking,   // engine::check_root (Modpacks da?)
    kStarting,   // Engine anlegen (Presenter-Thread, EGL, Shader)
    kRunning,    // engine.loop() läuft; bis zum ersten Bild = „erstes Bild“
    kFailed,     // Klartextfehler, kein Engine-Lauf
    kFinished,   // Engine regulär beendet (stop)
};
constexpr int kEnginePhaseCount = 7;

inline const char* enginePhaseName(EnginePhase p) {
    switch (p) {
    case EnginePhase::kIdle: return "wartet";
    case EnginePhase::kPreparing: return "Dateien entpacken";
    case EnginePhase::kChecking: return "Spieldaten prüfen";
    case EnginePhase::kStarting: return "Engine startet";
    case EnginePhase::kRunning: return "läuft";
    case EnginePhase::kFailed: return "Fehler";
    case EnginePhase::kFinished: return "beendet";
    }
    return "?";
}

struct EngineStatus {
    EnginePhase phase = EnginePhase::kIdle;
    double phaseSeconds = 0.0;                       // seit Beginn der aktuellen Phase
    double durations[kEnginePhaseCount] = {};        // Dauer abgeschlossener Phasen
    std::string error;                               // nur bei kFailed
};

// Eine Zeile des Ladebildschirms. kind: 0 Titelzeile, 1 Phase fertig, 2 Phase läuft, 3 Phase offen, 4 Fehler,
// 5 Hinweis.
struct LoaderLine {
    int kind = 0;
    std::string text;
};

inline std::string secondsText(double s) {
    char buf[32];
    if (s < 10.0) std::snprintf(buf, sizeof(buf), "%.1f s", s);
    else std::snprintf(buf, sizeof(buf), "%.0f s", s);
    std::string t = buf;
    for (char& c : t)
        if (c == '.') c = ',';  // deutsche Schreibweise
    return t;
}

// Text an Wortgrenzen auf höchstens maxChars Zeichen je Zeile umbrechen (UTF-8: zählt Bytes, reicht für Logtext).
inline std::vector<std::string> wrapText(const std::string& text, std::size_t maxChars) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&]() {
        while (word.size() > maxChars) {  // überlange Wörter (Pfade) hart teilen
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(word.substr(0, maxChars));
            word.erase(0, maxChars);
        }
        if (word.empty()) return;
        if (line.empty()) line = word;
        else if (line.size() + 1 + word.size() <= maxChars) line += " " + word;
        else {
            lines.push_back(line);
            line = word;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            flushWord();
            if (c == '\n' && !line.empty()) {
                lines.push_back(line);
                line.clear();
            }
        } else {
            word += c;
        }
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

struct LoaderTextParams {
    std::string logHint = "adb logcat -s xr.ages";  // wo das Log steht
    std::string importHint = "bash quest/push-data.sh";
    std::size_t wrap = 64;                          // Zeichen je Fehlerzeile
    std::string title = "Spiel wird geladen …";     // Titelzeile (Neustart: „Neue Karte wird erzeugt …“)
};

// Zeilen des Ladebildschirms: Phasenliste mit Laufzeit (fertig/läuft/offen) und bei kFailed der Klartextfehler mit
// Log- und Import-Hinweis. In kRunning ist phaseSeconds die Wartezeit auf das erste Bild (danach zeigt die
// XR-Schicht das Spielbild statt des Ladebildschirms).
inline std::vector<LoaderLine> loaderLines(const EngineStatus& s, const LoaderTextParams& p = {}) {
    std::vector<LoaderLine> out;
    const EnginePhase steps[] = {EnginePhase::kPreparing, EnginePhase::kChecking, EnginePhase::kStarting,
                                 EnginePhase::kRunning};
    const char* labels[] = {"Dateien entpacken", "Spieldaten prüfen", "Engine startet", "erstes Bild"};
    const bool failed = s.phase == EnginePhase::kFailed;
    // Fehlerphase = erste Phase ohne Dauer (dort ist es passiert)
    int failedAt = -1;
    if (failed) {
        for (int i = 0; i < 4; ++i)
            if (s.durations[static_cast<int>(steps[i])] <= 0.0) {
                failedAt = i;
                break;
            }
    }
    out.push_back({0, failed ? "Start abgebrochen" : p.title});
    for (int i = 0; i < 4; ++i) {
        const int idx = static_cast<int>(steps[i]);
        LoaderLine l;
        if (s.phase == steps[i]) {
            l.kind = 2;
            l.text = std::string(labels[i]) + " … " + secondsText(s.phaseSeconds);
        } else if (s.durations[idx] > 0.0) {
            l.kind = 1;
            l.text = std::string(labels[i]) + " – " + secondsText(s.durations[idx]);
        } else if (failed && i == failedAt) {
            l.kind = 4;
            l.text = std::string(labels[i]) + " – fehlgeschlagen";
        } else {
            l.kind = 3;
            l.text = labels[i];
        }
        out.push_back(l);
    }
    if (failed) {
        for (const std::string& line : wrapText(s.error.empty() ? "unbekannter Fehler" : s.error, p.wrap))
            out.push_back({4, line});
        out.push_back({5, "Log: " + p.logHint});
        out.push_back({5, "Spieldaten importieren (am PC): " + p.importHint});
        out.push_back({5, "Beenden: linke Menütaste → Beenden"});
    }
    return out;
}

}  // namespace agesxr
