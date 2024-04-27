#ifndef _STACKMAGICQCUE_H_INCLUDED
#define _STACKMAGICQCUE_H_INCLUDED

// Includes:
#include "StackCue.h"

// StackMagicQ cue is a cue that interacts with ChamSys MagicQ software to allow
// control of playbacks and other features
struct StackMagicQCue
{
	// Superclass
	StackCue super;

	// The MagicQ tab
	GtkWidget *magicq_tab;

	// The UDP socket
	int sock;
};

// Functions: MagicQ cue functions
void stack_magicq_cue_register();

// Defines:
#define STACK_MAGICQ_CUE(_c) ((StackMagicQCue*)(_c))

#endif

