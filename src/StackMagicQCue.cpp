// Includes:
#include "StackApp.h"
#include "StackLog.h"
#include "StackMagicQCue.h"
#include "StackGtkEntryHelper.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <cmath>
#include <json/json.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// Global: A single instance of our builder so we don't have to keep reloading
// it every time we change the selected cue
static GtkBuilder *smc_builder = NULL;

// Global: A single instace of our icon
static GdkPixbuf *icon = NULL;

typedef enum MagicQOperation {
	MAGICQ_OPERATION_ACTIVATE,
	MAGICQ_OPERATION_RELEASE,
	MAGICQ_OPERATION_GO,
	MAGICQ_OPERATION_STOP,
	MAGICQ_OPERATION_SET_LEVEL,
	MAGICQ_OPERATION_JUMP_TO_CUE_ID,
} MagicQOperation;

// TODO: Put this into an app-wide settings UI
static int16_t stack_magicq_cue_get_osc_port()
{
	char *env = getenv("STACK_MAGICQ_OSC_PORT");
	if (env != NULL)
	{
		int16_t port = atoi(env);
		if (port >= 1 && port <= 65535)
		{
			return port;
		}
	}

	return 8000;
}

static bool stack_magicq_cue_update_error_state(StackMagicQCue *cue)
{
	int16_t playback = 0, level = 0;
	char buffer[64];
	char *cue_id = NULL;
	bool action_activate = false, action_level = false, action_go = false,
		 action_stop = false, action_jump = false, action_release = false, error = false;

	// Get the values from the properties
	stack_property_get_int16(stack_cue_get_property(STACK_CUE(cue), "playback"), STACK_PROPERTY_VERSION_DEFINED, &playback);
	stack_property_get_int16(stack_cue_get_property(STACK_CUE(cue), "level"), STACK_PROPERTY_VERSION_DEFINED, &level);
	stack_property_get_string(stack_cue_get_property(STACK_CUE(cue), "jump_cue_id"), STACK_PROPERTY_VERSION_DEFINED, &cue_id);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_activate"), STACK_PROPERTY_VERSION_DEFINED, &action_activate);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_level"), STACK_PROPERTY_VERSION_DEFINED, &action_level);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_go"), STACK_PROPERTY_VERSION_DEFINED, &action_go);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_stop"), STACK_PROPERTY_VERSION_DEFINED, &action_stop);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_jump"), STACK_PROPERTY_VERSION_DEFINED, &action_jump);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_release"), STACK_PROPERTY_VERSION_DEFINED, &action_release);

	// We must have a playback ID
	if (playback == 0)
	{
		error = true;
	}

	// We must have an action
	if (!action_activate && !action_level && !action_go && !action_stop && !action_jump && !action_release)
	{
		error = true;
	}

	// If we're jumping, we must have a cue number
	if (action_jump && strlen(cue_id) == 0)
	{
		error = true;
	}

	if (error)
	{
		stack_cue_set_state(STACK_CUE(cue), STACK_CUE_STATE_ERROR);
	}
	else
	{
		if (STACK_CUE(cue)->state == STACK_CUE_STATE_ERROR)
		{
			stack_cue_set_state(STACK_CUE(cue), STACK_CUE_STATE_STOPPED);
		}
	}

	return error;
}

static void stack_magicq_cue_ccb_action(StackProperty *property, StackPropertyVersion version, void *user_data)
{
	// If a defined-version property has changed, we should notify the cue list
	// that we're now different
	if (version == STACK_PROPERTY_VERSION_DEFINED)
	{
		StackMagicQCue* cue = STACK_MAGICQ_CUE(user_data);

		// Notify cue list that we've changed
		stack_cue_list_changed(STACK_CUE(cue)->parent, STACK_CUE(cue), property);

		// Update our error state
		stack_magicq_cue_update_error_state(cue);

		// Fire an updated-selected-cue signal to signal the UI to change (we might
		// have changed state)
		if (cue->magicq_tab)
		{
			StackAppWindow *window = (StackAppWindow*)gtk_widget_get_toplevel(GTK_WIDGET(cue->magicq_tab));
			g_signal_emit_by_name((gpointer)window, "update-selected-cue");
		}
	}
}

static void stack_magicq_cue_ccb_playback(StackProperty *property, StackPropertyVersion version, void *user_data)
{
	// If a defined-version property has changed, we should notify the cue list
	// that we're now different
	if (version == STACK_PROPERTY_VERSION_DEFINED)
	{
		StackMagicQCue* cue = STACK_MAGICQ_CUE(user_data);

		// Notify cue list that we've changed
		stack_cue_list_changed(STACK_CUE(cue)->parent, STACK_CUE(cue), property);

		// Update our error state
		stack_magicq_cue_update_error_state(cue);

		// Fire an updated-selected-cue signal to signal the UI to change (we might
		// have changed state)
		if (cue->magicq_tab)
		{
			StackAppWindow *window = (StackAppWindow*)gtk_widget_get_toplevel(GTK_WIDGET(cue->magicq_tab));
			g_signal_emit_by_name((gpointer)window, "update-selected-cue");

			int16_t playback;
			char buffer[32];
			stack_property_get_int16(property, STACK_PROPERTY_VERSION_DEFINED, &playback);
			snprintf(buffer, 32, "%d", playback);
			gtk_entry_set_text(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryPlayback")), buffer);
		}
	}
}

static void stack_magicq_cue_ccb_level(StackProperty *property, StackPropertyVersion version, void *user_data)
{
	// If a defined-version property has changed, we should notify the cue list
	// that we're now different
	if (version == STACK_PROPERTY_VERSION_DEFINED)
	{
		StackMagicQCue* cue = STACK_MAGICQ_CUE(user_data);

		// Notify cue list that we've changed
		stack_cue_list_changed(STACK_CUE(cue)->parent, STACK_CUE(cue), property);

		// Update our error state
		stack_magicq_cue_update_error_state(cue);

		// Fire an updated-selected-cue signal to signal the UI to change (we might
		// have changed state)
		if (cue->magicq_tab)
		{
			StackAppWindow *window = (StackAppWindow*)gtk_widget_get_toplevel(GTK_WIDGET(cue->magicq_tab));
			g_signal_emit_by_name((gpointer)window, "update-selected-cue");

			int16_t level;
			char buffer[32];
			stack_property_get_int16(property, STACK_PROPERTY_VERSION_DEFINED, &level);
			snprintf(buffer, 32, "%d", level);
			gtk_entry_set_text(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryLevel")), buffer);
		}
	}
}

static void stack_magicq_cue_ccb_jump_cue_id(StackProperty *property, StackPropertyVersion version, void *user_data)
{
	// If a defined-version property has changed, we should notify the cue list
	// that we're now different
	if (version == STACK_PROPERTY_VERSION_DEFINED)
	{
		StackMagicQCue* cue = STACK_MAGICQ_CUE(user_data);

		// Notify cue list that we've changed
		stack_cue_list_changed(STACK_CUE(cue)->parent, STACK_CUE(cue), property);

		// Update our error state
		stack_magicq_cue_update_error_state(cue);

		// Fire an updated-selected-cue signal to signal the UI to change (we might
		// have changed state)
		if (cue->magicq_tab)
		{
			StackAppWindow *window = (StackAppWindow*)gtk_widget_get_toplevel(GTK_WIDGET(cue->magicq_tab));
			g_signal_emit_by_name((gpointer)window, "update-selected-cue");
		}
	}
}

int16_t stack_magicq_cue_validate_playback(StackPropertyInt16 *property, StackPropertyVersion version, const int16_t value, void *user_data)
{
	if (version == STACK_PROPERTY_VERSION_DEFINED)
	{
		if (value < 0)
		{
			return 0;
		}
		else if (value > 10)
		{
			return 10;
		}
	}

	return value;
}

int16_t stack_magicq_cue_validate_level(StackPropertyInt16 *property, StackPropertyVersion version, const int16_t value, void *user_data)
{
	if (version == STACK_PROPERTY_VERSION_DEFINED)
	{
		if (value < 0)
		{
			return 0;
		}
		else if (value > 100)
		{
			return 100;
		}
	}

	return value;
}
// TODO: Validate cue ID format?

/// Pause or resumes change callbacks on variables
static void stack_magicq_cue_pause_change_callbacks(StackCue *cue, bool pause)
{
	stack_property_pause_change_callback(stack_cue_get_property(cue, "playback"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "action_activate"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "action_level"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "level"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "action_go"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "action_stop"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "action_jump"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "jump_cue_id"), pause);
	stack_property_pause_change_callback(stack_cue_get_property(cue, "action_release"), pause);
}

////////////////////////////////////////////////////////////////////////////////
// CREATION AND DESTRUCTION

/// Creates a MagicQ cue
static StackCue* stack_magicq_cue_create(StackCueList *cue_list)
{
	// Allocate the cue
	StackMagicQCue* cue = new StackMagicQCue();

	// Initialise the superclass
	stack_cue_init(&cue->super, cue_list);

	// Make this class a StackMagicQCue
	cue->super._class_name = "StackMagicQCue";

	// We start in error state until we have a target
	stack_cue_set_state(STACK_CUE(cue), STACK_CUE_STATE_ERROR);

	// Initialise our variables
	cue->magicq_tab = NULL;
	cue->sock = 0;
	stack_cue_set_action_time(STACK_CUE(cue), 1);

	// Add our properties
	StackProperty *playback = stack_property_create("playback", STACK_PROPERTY_TYPE_INT16);
	stack_cue_add_property(STACK_CUE(cue), playback);
	stack_property_set_changed_callback(playback, stack_magicq_cue_ccb_playback, (void*)cue);
	stack_property_set_validator(playback, (stack_property_validator_t)stack_magicq_cue_validate_playback, (void*)cue);

	StackProperty *action_activate = stack_property_create("action_activate", STACK_PROPERTY_TYPE_BOOL);
	stack_cue_add_property(STACK_CUE(cue), action_activate);
	stack_property_set_changed_callback(action_activate, stack_magicq_cue_ccb_action, (void*)cue);

	StackProperty *action_level = stack_property_create("action_level", STACK_PROPERTY_TYPE_BOOL);
	stack_cue_add_property(STACK_CUE(cue), action_level);
	stack_property_set_changed_callback(action_level, stack_magicq_cue_ccb_action, (void*)cue);

	StackProperty *level = stack_property_create("level", STACK_PROPERTY_TYPE_INT16);
	stack_cue_add_property(STACK_CUE(cue), level);
	stack_property_set_changed_callback(level, stack_magicq_cue_ccb_level, (void*)cue);
	stack_property_set_validator(level, (stack_property_validator_t)stack_magicq_cue_validate_level, (void*)cue);

	StackProperty *action_go = stack_property_create("action_go", STACK_PROPERTY_TYPE_BOOL);
	stack_cue_add_property(STACK_CUE(cue), action_go);
	stack_property_set_changed_callback(action_go, stack_magicq_cue_ccb_action, (void*)cue);

	StackProperty *action_stop = stack_property_create("action_stop", STACK_PROPERTY_TYPE_BOOL);
	stack_cue_add_property(STACK_CUE(cue), action_stop);
	stack_property_set_changed_callback(action_stop, stack_magicq_cue_ccb_action, (void*)cue);

	StackProperty *action_jump = stack_property_create("action_jump", STACK_PROPERTY_TYPE_BOOL);
	stack_cue_add_property(STACK_CUE(cue), action_jump);
	stack_property_set_changed_callback(action_jump, stack_magicq_cue_ccb_action, (void*)cue);

	StackProperty *jump_cue_id = stack_property_create("jump_cue_id", STACK_PROPERTY_TYPE_STRING);
	stack_cue_add_property(STACK_CUE(cue), jump_cue_id);
	stack_property_set_changed_callback(jump_cue_id, stack_magicq_cue_ccb_jump_cue_id, (void*)cue);

	StackProperty *action_release = stack_property_create("action_release", STACK_PROPERTY_TYPE_BOOL);
	stack_cue_add_property(STACK_CUE(cue), action_release);
	stack_property_set_changed_callback(action_release, stack_magicq_cue_ccb_action, (void*)cue);

	// Initialise superclass variables
	stack_cue_set_name(STACK_CUE(cue), "MagicQ Action");

	return STACK_CUE(cue);
}

/// Destroys a MagicQ cue
static void stack_magicq_cue_destroy(StackCue *cue)
{
	// Tidy up the socket
	if (STACK_MAGICQ_CUE(cue)->sock > 0)
	{
		close(STACK_MAGICQ_CUE(cue)->sock);
	}

	// Call parent destructor
	stack_cue_destroy_base(cue);
}

////////////////////////////////////////////////////////////////////////////////
// PROPERTY SETTERS

////////////////////////////////////////////////////////////////////////////////
// UI CALLBACKS

extern "C" void mcp_action_toggled(GtkToggleButton *toggle_button, gpointer user_data)
{
	StackCue *cue = STACK_CUE(((StackAppWindow*)gtk_widget_get_toplevel(GTK_WIDGET(toggle_button)))->selected_cue);
	const gchar *name = gtk_widget_get_name(GTK_WIDGET(toggle_button));
	bool active = gtk_toggle_button_get_active(toggle_button);

	// Figure out the property name
	char property_name[64];
	snprintf(property_name, 64, "action_%s", name);

	// Set the property value
	stack_property_set_bool(stack_cue_get_property(cue, property_name), STACK_PROPERTY_VERSION_DEFINED, active);
}

extern "C" gboolean mcp_playback_changed(GtkWidget *widget, gpointer user_data)
{
	StackCue *cue = STACK_CUE(((StackAppWindow*)gtk_widget_get_toplevel(widget))->selected_cue);
	const gchar *value = gtk_entry_get_text(GTK_ENTRY(widget));
	int16_t playback = (int16_t)atoi(value);
	stack_property_set_int16(stack_cue_get_property(cue, "playback"), STACK_PROPERTY_VERSION_DEFINED, playback);
	return false;
}

extern "C" gboolean mcp_level_changed(GtkWidget *widget, gpointer user_data)
{
	StackCue *cue = STACK_CUE(((StackAppWindow*)gtk_widget_get_toplevel(widget))->selected_cue);
	const gchar *value = gtk_entry_get_text(GTK_ENTRY(widget));
	int16_t level = (int16_t)atoi(value);
	stack_property_set_int16(stack_cue_get_property(cue, "level"), STACK_PROPERTY_VERSION_DEFINED, level);
	return false;
}

extern "C" gboolean mcp_jump_cue_id_changed(GtkWidget *widget, gpointer user_data)
{
	StackCue *cue = STACK_CUE(((StackAppWindow*)gtk_widget_get_toplevel(widget))->selected_cue);
	const gchar *value = gtk_entry_get_text(GTK_ENTRY(widget));
	stack_property_set_string(stack_cue_get_property(cue, "jump_cue_id"), STACK_PROPERTY_VERSION_DEFINED, value);
	return false;
}

////////////////////////////////////////////////////////////////////////////////
// MAGICQ OPERATIONS

static bool stack_magicq_cue_establish_socket(StackMagicQCue *cue)
{
	// Don't do anything if we've already got a socket
	if (cue->sock > 0)
	{
		return true;
	}

	stack_log("stack_magicq_cue_establish_socket(): Establishing new socket (%d)\n", cue->sock);

	// Create our UDP socket
	cue->sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (cue->sock <= 0)
	{
		stack_log("stack_magicq_cue_establish_socket(): Failed to create socket (%d)\n", cue->sock);
		cue->sock = 0;
		return false;
	}

	// Bind it somewhere locally
	struct sockaddr_in source;
	source.sin_family = AF_INET;
	source.sin_port = htons(stack_magicq_cue_get_osc_port());
	source.sin_addr.s_addr = htonl(INADDR_ANY);
	int res = bind(cue->sock, (struct sockaddr *)&source, sizeof(source));
	if (res == 0)
	{
		stack_log("stack_magicq_cue_establish_socket(): Failed to bind socket (%d)\n", res);
		close(cue->sock);
		cue->sock = 0;
		return false;
	}

	return true;
}

static bool stack_magicq_cue_send_osc_packet(StackMagicQCue *cue, MagicQOperation operation)
{
	// Note that this buffer must be at least eight bytes larger than our biggest
	// command
	char buffer[64];
	memset(buffer, 0, 64);

	// Get the values from the properties
	int16_t playback = 0, level = 0;
	char *cue_id = NULL;
	stack_property_get_int16(stack_cue_get_property(STACK_CUE(cue), "playback"), STACK_PROPERTY_VERSION_LIVE, &playback);

	switch (operation)
	{
		case MAGICQ_OPERATION_ACTIVATE:
			snprintf(buffer, 32, "/rpc/%dA", playback);
			break;
		case MAGICQ_OPERATION_RELEASE:
			snprintf(buffer, 32, "/rpc/%dR", playback);
			break;
		case MAGICQ_OPERATION_GO:
			snprintf(buffer, 32, "/rpc/%dG", playback);
			break;
		case MAGICQ_OPERATION_STOP:
			snprintf(buffer, 32, "/rpc/%dS", playback);
			break;
		case MAGICQ_OPERATION_SET_LEVEL:
			stack_property_get_int16(stack_cue_get_property(STACK_CUE(cue), "level"), STACK_PROPERTY_VERSION_LIVE, &level);
			snprintf(buffer, 32, "/rpc/%d,%dL", playback, level);
			break;
		case MAGICQ_OPERATION_JUMP_TO_CUE_ID:
			stack_property_get_string(stack_cue_get_property(STACK_CUE(cue), "jump_cue_id"), STACK_PROPERTY_VERSION_LIVE, &cue_id);
			snprintf(buffer, 32, "/rpc/%d,%sJ", playback, cue_id);
			break;
	}

	// Get the length of the commad (plus one for the NUL terminator)
	size_t length = strlen(buffer) + 1;

	// Pad to nearest four bytes as per the OSC protocol
	length += length % 4;

	// Add on the comma (plus a implied three NULs padding) as per the OSC
	// protocol to imply zero arguments
	snprintf(&buffer[length], 32 - length, ",");
	length += 4;

	// Ensure the socket is ready
	if (!stack_magicq_cue_establish_socket(cue))
	{
		return false;
	}

	// Set destination
	struct sockaddr_in dest;
	dest.sin_family = AF_INET;
	dest.sin_port = htons(stack_magicq_cue_get_osc_port());
	dest.sin_addr.s_addr = htonl(0x7f000001); // TODO Configurable

	// Send datagram
	int s = sendto(cue->sock, buffer, length, 0, (struct sockaddr *)&dest, sizeof(dest));
	if (s <= 0)
	{
		stack_log("stack_magicq_cue_send_osc_packet(): Failed to send datagram (%d)\n", s);

		// Close the socket so we will re-establish on next attempt
		close(cue->sock);
		cue->sock = 0;
		return false;
	}

	return true;
}

////////////////////////////////////////////////////////////////////////////////
// BASE CUE OPERATIONS

/// Start the cue playing
static bool stack_magicq_cue_play(StackCue *cue)
{
	// Call the superclass
	if (!stack_cue_play_base(cue))
	{
		return false;
	}

	// Double-check our error state and don't play if we're broken
	if (stack_magicq_cue_update_error_state(STACK_MAGICQ_CUE(cue)))
	{
		return false;
	}

	// Copy the variables to live
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "playback"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "action_activate"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "action_level"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "level"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "action_go"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "action_stop"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "action_jump"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "jump_cue_id"));
	stack_property_copy_defined_to_live(stack_cue_get_property(cue, "action_release"));

	return true;
}

/// Update the cue based on time
static void stack_magicq_cue_pulse(StackCue *cue, stack_time_t clocktime)
{
	bool this_action = false;
	// Get the cue state before the base class potentially updates it
	StackCueState pre_pulse_state = cue->state;

	// Call superclass
	stack_cue_pulse_base(cue, clocktime);

	// We have zero action time so we need to detect the transition from pre->something or playing->something
	if ((pre_pulse_state == STACK_CUE_STATE_PLAYING_PRE && cue->state != STACK_CUE_STATE_PLAYING_PRE) ||
		(pre_pulse_state == STACK_CUE_STATE_PLAYING_ACTION && cue->state != STACK_CUE_STATE_PLAYING_ACTION))
	{
		this_action = false;
		stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_activate"), STACK_PROPERTY_VERSION_LIVE, &this_action);
		if (this_action)
		{
			stack_magicq_cue_send_osc_packet(STACK_MAGICQ_CUE(cue), MAGICQ_OPERATION_ACTIVATE);
		}

		this_action = false;
		stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_level"), STACK_PROPERTY_VERSION_LIVE, &this_action);
		if (this_action)
		{
			stack_magicq_cue_send_osc_packet(STACK_MAGICQ_CUE(cue), MAGICQ_OPERATION_SET_LEVEL);
		}

		this_action = false;
		stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_go"), STACK_PROPERTY_VERSION_LIVE, &this_action);
		if (this_action)
		{
			stack_magicq_cue_send_osc_packet(STACK_MAGICQ_CUE(cue), MAGICQ_OPERATION_GO);
		}

		this_action = false;
		stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_jump"), STACK_PROPERTY_VERSION_LIVE, &this_action);
		if (this_action)
		{
			stack_magicq_cue_send_osc_packet(STACK_MAGICQ_CUE(cue), MAGICQ_OPERATION_JUMP_TO_CUE_ID);
		}

		this_action = false;
		stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_stop"), STACK_PROPERTY_VERSION_LIVE, &this_action);
		if (this_action)
		{
			stack_magicq_cue_send_osc_packet(STACK_MAGICQ_CUE(cue), MAGICQ_OPERATION_ACTIVATE);
		}

		this_action = false;
		stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_release"), STACK_PROPERTY_VERSION_LIVE, &this_action);
		if (this_action)
		{
			stack_magicq_cue_send_osc_packet(STACK_MAGICQ_CUE(cue), MAGICQ_OPERATION_RELEASE);
		}
	}
}

/// Sets up the tabs for the action cue
static void stack_magicq_cue_set_tabs(StackCue *cue, GtkNotebook *notebook)
{
	StackMagicQCue *acue = STACK_MAGICQ_CUE(cue);

	// Create the tab
	GtkWidget *label = gtk_label_new("MagicQ");

	// Load the UI (if we haven't already)
	if (smc_builder == NULL)
	{
		smc_builder = gtk_builder_new_from_resource("/org/stack/ui/StackMagicQCue.ui");

		stack_limit_gtk_entry_float(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryCueID")), false);
		stack_limit_gtk_entry_int(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryPlayback")), false);
		stack_limit_gtk_entry_int(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryLevel")), false);

		// Set up callbacks
		gtk_builder_add_callback_symbol(smc_builder, "mcp_action_toggled", G_CALLBACK(mcp_action_toggled));
		gtk_builder_add_callback_symbol(smc_builder, "mcp_playback_changed", G_CALLBACK(mcp_playback_changed));
		gtk_builder_add_callback_symbol(smc_builder, "mcp_level_changed", G_CALLBACK(mcp_level_changed));
		gtk_builder_add_callback_symbol(smc_builder, "mcp_jump_cue_id_changed", G_CALLBACK(mcp_jump_cue_id_changed));

		// Connect the signals
		gtk_builder_connect_signals(smc_builder, NULL);
	}
	acue->magicq_tab = GTK_WIDGET(gtk_builder_get_object(smc_builder, "mcpGrid"));

	// Pause change callbacks on the properties
	stack_magicq_cue_pause_change_callbacks(cue, true);

	// Add an extra reference to the action tab - we're about to remove it's
	// parent and we don't want it to get garbage collected
	g_object_ref(acue->magicq_tab);

	// The tab has a parent window in the UI file - unparent the tab container from it
	gtk_widget_unparent(acue->magicq_tab);

	// Append the tab (and show it, because it starts off hidden...)
	gtk_notebook_append_page(notebook, acue->magicq_tab, label);
	gtk_widget_show(acue->magicq_tab);

	int16_t playback = 0, level = 0;
	char buffer[64];
	char *cue_id = NULL;
	bool action_activate = false, action_level = false, action_go = false,
		 action_stop = false, action_jump = false, action_release = false;

	// Get the values from the properties
	stack_property_get_int16(stack_cue_get_property(cue, "playback"), STACK_PROPERTY_VERSION_DEFINED, &playback);
	stack_property_get_int16(stack_cue_get_property(cue, "level"), STACK_PROPERTY_VERSION_DEFINED, &level);
	stack_property_get_string(stack_cue_get_property(cue, "jump_cue_id"), STACK_PROPERTY_VERSION_DEFINED, &cue_id);
	stack_property_get_bool(stack_cue_get_property(cue, "action_activate"), STACK_PROPERTY_VERSION_DEFINED, &action_activate);
	stack_property_get_bool(stack_cue_get_property(cue, "action_level"), STACK_PROPERTY_VERSION_DEFINED, &action_level);
	stack_property_get_bool(stack_cue_get_property(cue, "action_go"), STACK_PROPERTY_VERSION_DEFINED, &action_go);
	stack_property_get_bool(stack_cue_get_property(cue, "action_stop"), STACK_PROPERTY_VERSION_DEFINED, &action_stop);
	stack_property_get_bool(stack_cue_get_property(cue, "action_jump"), STACK_PROPERTY_VERSION_DEFINED, &action_jump);
	stack_property_get_bool(stack_cue_get_property(cue, "action_release"), STACK_PROPERTY_VERSION_DEFINED, &action_release);

	// Set all the values
	snprintf(buffer, 64, "%d", playback);
	gtk_entry_set_text(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryPlayback")), buffer);
	snprintf(buffer, 64, "%d", level);
	gtk_entry_set_text(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryLevel")), buffer);
	gtk_entry_set_text(GTK_ENTRY(gtk_builder_get_object(smc_builder, "mcpEntryCueID")), cue_id);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_builder_get_object(smc_builder, "mcpCheckActivate")), action_activate);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_builder_get_object(smc_builder, "mcpCheckLevel")), action_level);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_builder_get_object(smc_builder, "mcpCheckGo")), action_go);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_builder_get_object(smc_builder, "mcpCheckStop")), action_stop);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_builder_get_object(smc_builder, "mcpCheckJump")), action_jump);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gtk_builder_get_object(smc_builder, "mcpCheckRelease")), action_release);

	// Resume change callbacks on the properties
	stack_magicq_cue_pause_change_callbacks(cue, false);
}

/// Removes the properties tabs for a action cue
static void stack_magicq_cue_unset_tabs(StackCue *cue, GtkNotebook *notebook)
{
	// Find our media page
	gint page = gtk_notebook_page_num(notebook, STACK_MAGICQ_CUE(cue)->magicq_tab);

	// If we've found the page, remove it
	if (page >= 0)
	{
		gtk_notebook_remove_page(notebook, page);
	}

	// Remove our reference to the action tab
	g_object_unref(STACK_MAGICQ_CUE(cue)->magicq_tab);

	// Be tidy
	STACK_MAGICQ_CUE(cue)->magicq_tab = NULL;
}

/// Saves the details of this cue as JSON
static char *stack_magicq_cue_to_json(StackCue *cue)
{
	StackMagicQCue *acue = STACK_MAGICQ_CUE(cue);

	// Build JSON
	Json::Value cue_root;

	// Write out our properties
	stack_property_write_json(stack_cue_get_property(cue, "playback"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "action_activate"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "action_level"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "level"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "action_go"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "action_stop"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "action_jump"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "jump_cue_id"), &cue_root);
	stack_property_write_json(stack_cue_get_property(cue, "action_release"), &cue_root);

	// Write out JSON string and return (to be free'd by
	// stack_magicq_cue_free_json)
	Json::FastWriter writer;
	return strdup(writer.write(cue_root).c_str());
}

/// Frees JSON strings as returned by stack_magicq_cue_to_json
static void stack_magicq_cue_free_json(StackCue *cue, char *json_data)
{
	free(json_data);
}

/// Re-initialises this cue from JSON Data
void stack_magicq_cue_from_json(StackCue *cue, const char *json_data)
{
	Json::Value cue_root;
	Json::Reader reader;

	// Parse JSON data
	reader.parse(json_data, json_data + strlen(json_data), cue_root, false);

	// Get the data that's pertinent to us
	if (!cue_root.isMember("StackMagicQCue"))
	{
		stack_log("stack_magicq_cue_from_json(): Missing StackMagicQCue class\n");
		return;
	}

	Json::Value& cue_data = cue_root["StackMagicQCue"];

	// Read in our properties
	if (cue_data.isMember("playback"))
	{
		stack_property_set_int16(stack_cue_get_property(cue, "playback"), STACK_PROPERTY_VERSION_DEFINED, cue_data["playback"].asInt());
	}

	if (cue_data.isMember("action_activate"))
	{
		stack_property_set_bool(stack_cue_get_property(cue, "action_activate"), STACK_PROPERTY_VERSION_DEFINED, cue_data["action_activate"].asBool());
	}

	if (cue_data.isMember("action_level"))
	{
		stack_property_set_bool(stack_cue_get_property(cue, "action_level"), STACK_PROPERTY_VERSION_DEFINED, cue_data["action_level"].asBool());
	}

	if (cue_data.isMember("level"))
	{
		stack_property_set_int16(stack_cue_get_property(cue, "level"), STACK_PROPERTY_VERSION_DEFINED, cue_data["level"].asInt());
	}

	if (cue_data.isMember("action_go"))
	{
		stack_property_set_bool(stack_cue_get_property(cue, "action_go"), STACK_PROPERTY_VERSION_DEFINED, cue_data["action_go"].asBool());
	}

	if (cue_data.isMember("action_stop"))
	{
		stack_property_set_bool(stack_cue_get_property(cue, "action_stop"), STACK_PROPERTY_VERSION_DEFINED, cue_data["action_stop"].asBool());
	}

	if (cue_data.isMember("action_jump"))
	{
		stack_property_set_bool(stack_cue_get_property(cue, "action_jump"), STACK_PROPERTY_VERSION_DEFINED, cue_data["action_jump"].asBool());
	}

	if (cue_data.isMember("jump_cue_id"))
	{
		stack_property_set_string(stack_cue_get_property(cue, "jump_cue_id"), STACK_PROPERTY_VERSION_DEFINED, cue_data["jump_cue_id"].asString().c_str());
	}

	if (cue_data.isMember("action_release"))
	{
		stack_property_set_bool(stack_cue_get_property(cue, "action_release"), STACK_PROPERTY_VERSION_DEFINED, cue_data["action_release"].asBool());
	}

	stack_magicq_cue_update_error_state(STACK_MAGICQ_CUE(cue));
}

/// Gets the error message for the cue
void stack_magicq_cue_get_error(StackCue *cue, char *message, size_t size)
{
	int16_t playback = 0, level = 0;
	char buffer[64];
	char *cue_id = NULL;
	bool action_activate = false, action_level = false, action_go = false,
		 action_stop = false, action_jump = false, action_release = false;

	// Get the values from the properties
	stack_property_get_int16(stack_cue_get_property(STACK_CUE(cue), "playback"), STACK_PROPERTY_VERSION_DEFINED, &playback);
	stack_property_get_int16(stack_cue_get_property(STACK_CUE(cue), "level"), STACK_PROPERTY_VERSION_DEFINED, &level);
	stack_property_get_string(stack_cue_get_property(STACK_CUE(cue), "jump_cue_id"), STACK_PROPERTY_VERSION_DEFINED, &cue_id);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_activate"), STACK_PROPERTY_VERSION_DEFINED, &action_activate);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_level"), STACK_PROPERTY_VERSION_DEFINED, &action_level);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_go"), STACK_PROPERTY_VERSION_DEFINED, &action_go);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_stop"), STACK_PROPERTY_VERSION_DEFINED, &action_stop);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_jump"), STACK_PROPERTY_VERSION_DEFINED, &action_jump);
	stack_property_get_bool(stack_cue_get_property(STACK_CUE(cue), "action_release"), STACK_PROPERTY_VERSION_DEFINED, &action_release);

	// We must have a playback ID
	if (playback == 0)
	{
		snprintf(message, size, "No playback chosen");
		return;
	}

	// We must have an action
	if (!action_activate && !action_level && !action_go && !action_stop && !action_jump && !action_release)
	{
		snprintf(message, size, "No actions selected");
		return;
	}

	// If we're jumping, we must have a cue number
	if (action_jump && strlen(cue_id) == 0)
	{
		snprintf(message, size, "No cue chosen to jump to");
		return;
	}

	// No error
	strncpy(message, "", size);
}

const char *stack_magicq_cue_get_field(StackCue *cue, const char *field)
{
	if (strcmp(field, "playback") == 0)
	{
		int16_t playback = 0;
		stack_property_get_int16(stack_cue_get_property(cue, "playback"), STACK_PROPERTY_VERSION_DEFINED, &playback);
		snprintf(STACK_MAGICQ_CUE(cue)->playback_string, 8, "%d", playback);
		return STACK_MAGICQ_CUE(cue)->playback_string;
	}
	else if (strcmp(field, "level") == 0)
	{
		int16_t level = 0;
		stack_property_get_int16(stack_cue_get_property(cue, "level"), STACK_PROPERTY_VERSION_DEFINED, &level);
		snprintf(STACK_MAGICQ_CUE(cue)->level_string, 8, "%d", level);
		return STACK_MAGICQ_CUE(cue)->level_string;
	}
	else if (strcmp(field, "jump_target") == 0)
	{
		char *jump_target = NULL;
		stack_property_get_string(stack_cue_get_property(cue, "jump_target"), STACK_PROPERTY_VERSION_DEFINED, &jump_target);
		return jump_target;
	}

	return stack_cue_get_field_base(cue, field);
}

/// Returns the icon for a cue
/// @param cue The cue to get the icon of
GdkPixbuf *stack_magicq_cue_get_icon(StackCue *cue)
{
	return icon;
}

////////////////////////////////////////////////////////////////////////////////
// CLASS REGISTRATION

// Registers StackMagicQCue with the application
void stack_magicq_cue_register()
{
	// Load the icons
	icon = gdk_pixbuf_new_from_resource("/org/stack/icons/stackmagicqcue.png", NULL);

	// Register built in cue types
	StackCueClass* magicq_cue_class = new StackCueClass{ "StackMagicQCue", "StackCue", "MagicQ Cue", stack_magicq_cue_create, stack_magicq_cue_destroy, stack_magicq_cue_play, NULL, NULL, stack_magicq_cue_pulse, stack_magicq_cue_set_tabs, stack_magicq_cue_unset_tabs, stack_magicq_cue_to_json, stack_magicq_cue_free_json, stack_magicq_cue_from_json, stack_magicq_cue_get_error, NULL, NULL, stack_magicq_cue_get_field, stack_magicq_cue_get_icon, NULL, NULL };
	stack_register_cue_class(magicq_cue_class);
}

// The entry point for the plugin that Stack calls
extern "C" bool stack_init_plugin()
{
	stack_magicq_cue_register();
	return true;
}
