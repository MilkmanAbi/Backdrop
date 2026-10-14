// Backdrop colour filters: a 5x5 colour matrix applied to the whole screen by DWM (Magnification API),
// blended toward "no change" by an intensity, with fades, sign-in default and a daily schedule.
#pragma once
#include <windows.h>

enum { FILTER_COUNT = 11 };
const wchar_t* FilterName(int type);
// rows = output channel; intensity 0..1 blends from identity to the full filter
void FilterMatrix(int type, float intensity, float m[3][3], float off[3]);

struct FilterSettings {
    int type = 0;                      // Sepia
    int intensity = 70;                // percent
    bool atLogin = false;              // turn on when signing in
    bool schedule = false;             // daily on/off times
    int from = 21 * 60, to = 7 * 60;   // minutes after local midnight
};
extern FilterSettings g_fs;
extern bool g_filterOn;
extern void (*g_filterChanged)();      // UI refresh hook (tray tooltip, panel)

void FilterSave();
void FilterStartup();                  // read settings, decide sign-in state, apply instantly
void FilterSetOn(bool on, int fadeMs); // fades from whatever is on screen now
void FilterUpdate(int fadeMs);         // type/intensity changed, or re-push after display/session changes
void FilterShutdown();                 // back to normal colours, release the effect

bool ScheduleActiveNow();
void ScheduleFollow(int fadeMs);       // apply the schedule's state now (schedule switched on / times edited)
void ScheduleCheck(int fadeMs);        // only acts when an on/off time has passed (keeps manual overrides)
void ScheduleArm();                    // timer for the next on/off time
