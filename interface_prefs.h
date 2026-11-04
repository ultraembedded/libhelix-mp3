#ifndef MINTAMP_INTERFACE_PREFS_H
#define MINTAMP_INTERFACE_PREFS_H
#include "launch_options.h"
struct Window;
void mint_interface_load(MintLaunchOptions *o);
void mint_interface_tooltypes(MintLaunchOptions *o,int argc,char **argv);
/* Shared native requester; saves choices for MintAMP-Launch's next launch. */
void mint_interface_preferences(struct Window *parent);
void mint_interface_flush(void);
/* Skin-capable editions: CLI > own icon ToolTypes > shared MintAMP2/MODE >
 * skinned default. Returns 1 to open the skin player first, 0 for classic. */
int mint_interface_start_skinned(int argc,char **argv);
/* Remember a live Skin/Classic switch for the next launch. ENV now; ENVARC on
 * clean shutdown through mint_interface_flush(), after audio retirement. */
void mint_interface_set_mode(int skinned);
void mint_interface_flush_skin(void);
#endif
