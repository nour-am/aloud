#ifndef ALOUD_DEMO_H
#define ALOUD_DEMO_H

/* Scripted UI + audio tour for the README demo video (simulator build only).
 * Writes PNG frames, audio.wav and timeline.txt into out_dir. Returns 0 or -errno. */
int demo_run(const char *lib_dir, const char *state_dir, const char *font_dir,
             const char *out_dir);

#endif
