/* ************************************************************************
*   File: olc.trigger.c                                   EmpireMUD 2.0b1 *
*  Usage: OLC for triggers                                                *
*                                                                         *
*  EmpireMUD code base by Paul Clarke, (C) 2000-2015                      *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  EmpireMUD based upon CircleMUD 3.0, bpl 17, by Jeremy Elson.           *
*  CircleMUD (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
************************************************************************ */
#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "interpreter.h"
#include "db.h"
#include "comm.h"
#include "olc.h"
#include "handler.h"
#include "dg_scripts.h"
#include "dg_event.h"

/**
* Contents:
*   Helpers
*   Displays
*   Edit Modules
*/

// external consts
extern const char **trig_attach_type_list[];
extern const char *trig_attach_types[];


// external funcs
void trig_data_init(trig_data *this_data);


// locals
const char *trig_arg_obj_where[] = {
	"equipment",
	"inventory",
	"room",
	"\n"
};

const char *trig_arg_phrase_type[] = {
	"phrase",
	"wordlist",
	"\n"
};


 //////////////////////////////////////////////////////////////////////////////
//// HELPERS /////////////////////////////////////////////////////////////////

/**
* Determines all the argument types to show for the various types of a trigger.
*
* @param trig_data *trig The trigger.
* @return bitvector_t A set of TRIG_ARG_x flags.
*/
bitvector_t compile_argument_types_for_trigger(trig_data *trig) {
	extern const bitvector_t *trig_argument_type_list[];
	
	bitvector_t flags = NOBITS, trigger_types = GET_TRIG_TYPE(trig);
	int pos;
	
	for (pos = 0; trigger_types; trigger_types >>= 1, ++pos) {
		if (IS_SET(trigger_types, BIT(0))) {
			flags |= trig_argument_type_list[trig->attach_type][pos];
		}
	}
	
	return flags;
}


/**
* Creates a new trigger entry.
* 
* @param trig_vnum vnum The number to create.
* @return trig_data* The new trigger.
*/
trig_data *create_trigger_table_entry(trig_vnum vnum) {
	void add_trigger_to_table(trig_data *trig);
	
	trig_data *trig;
	
	// sanity
	if (real_trigger(vnum)) {
		log("SYSERR: Attempting to insert trigger at existing vnum %d", vnum);
		return real_trigger(vnum);
	}
	
	CREATE(trig, trig_data, 1);
	trig_data_init(trig);
	trig->vnum = vnum;
	add_trigger_to_table(trig);
	
	// simple default data (triggers cannot be nameless)
	trig->name = str_dup("New Trigger");
		
	// save index and crop file now
	save_index(DB_BOOT_TRG);
	save_library_file_for_vnum(DB_BOOT_TRG, vnum);
	
	return trig;
}


/**
* Removes triggers from a live set of scripts, by vnum.
*
* @param struct script_data *script The script to remove triggers from.
* @param trig_vnum vnum The trigger vnum to remove.
* @return bool TRUE if any were removed; FALSE otherwise.
*/
bool remove_live_script_by_vnum(struct script_data *script, trig_vnum vnum) {
	void extract_trigger(trig_data *trig);
	
	struct trig_data *trig, *next_trig, *temp;
	bool found = FALSE;
	
	if (!script) {
		return found;
	}

	for (trig = TRIGGERS(script); trig; trig = next_trig) {
		next_trig = trig->next;
		
		if (GET_TRIG_VNUM(trig) == vnum) {
			found = TRUE;
			REMOVE_FROM_LIST(trig, TRIGGERS(script), next);
			extract_trigger(trig);
		}
	}
	
	return found;
}


/**
* Deletes a trigger from a proto list.
*
* @param struct trig_proto_list **list The list to check.
* @param trig_vnum vnum The trigger to remove.
* @return bool TRUE if any triggers were removed, FALSE if not.
*/
bool delete_from_proto_list_by_vnum(struct trig_proto_list **list, trig_vnum vnum) {
	struct trig_proto_list *trig, *next_trig, *temp;
	bool found = FALSE;
	
	for (trig = *list; trig; trig = next_trig) {
		next_trig = trig->next;
		if (trig->vnum == vnum) {
			found = TRUE;
			REMOVE_FROM_LIST(trig, *list, next);
			free(trig);
		}
	}
	
	return found;
}


/**
* WARNING: This function actually deletes a trigger.
*
* @param char_data *ch The person doing the deleting.
* @param trig_vnum vnum The vnum to delete.
*/
void olc_delete_trigger(char_data *ch, trig_vnum vnum) {
	void remove_trigger_from_table(trig_data *trig);
	
	trig_data *trig;
	room_template *rmt, *next_rmt;
	room_data *room, *next_room;
	char_data *mob, *next_mob;
	descriptor_data *dsc;
	obj_data *obj, *next_obj;

	if (!(trig = real_trigger(vnum))) {
		msg_to_char(ch, "There is no such trigger %d.\r\n", vnum);
		return;
	}
	
	if (HASH_COUNT(trigger_table) <= 1) {
		msg_to_char(ch, "You can't delete the last trigger.\r\n");
		return;
	}
	
	// remove from hash table
	remove_trigger_from_table(trig);
	
	// look for live mobs with this script and remove
	for (mob = character_list; mob; mob = mob->next) {
		if (IS_NPC(mob) && SCRIPT(mob)) {
			remove_live_script_by_vnum(SCRIPT(mob), vnum);
		}
	}
	
	// look for live objs with this script and remove
	for (obj = object_list; obj; obj = obj->next) {
		if (SCRIPT(obj)) {
			remove_live_script_by_vnum(SCRIPT(obj), vnum);
		}
	}
	
	// look for live rooms with this trigger
	HASH_ITER(world_hh, world_table, room, next_room) {
		if (SCRIPT(room)) {
			remove_live_script_by_vnum(SCRIPT(room), vnum);
		}
		delete_from_proto_list_by_vnum(&(room->proto_script), vnum);
	}
	
	// update mob protos
	HASH_ITER(hh, mobile_table, mob, next_mob) {
		if (delete_from_proto_list_by_vnum(&mob->proto_script, vnum)) {
			save_library_file_for_vnum(DB_BOOT_MOB, mob->vnum);
		}
	}
	
	// update obj protos
	HASH_ITER(hh, object_table, obj, next_obj) {
		if (delete_from_proto_list_by_vnum(&obj->proto_script, vnum)) {
			save_library_file_for_vnum(DB_BOOT_OBJ, GET_OBJ_VNUM(obj));
		}
	}
	
	// room templates
	HASH_ITER(hh, room_template_table, rmt, next_rmt) {
		if (delete_from_proto_list_by_vnum(&GET_RMT_SCRIPTS(rmt), vnum)) {
			save_library_file_for_vnum(DB_BOOT_RMT, GET_RMT_VNUM(rmt));
		}
	}
	
	// update olc editors
	for (dsc = descriptor_list; dsc; dsc = dsc->next) {
		if (GET_OLC_OBJECT(dsc) && delete_from_proto_list_by_vnum(&GET_OLC_OBJECT(dsc)->proto_script, vnum)) {
			msg_to_char(dsc->character, "A trigger attached to the object you're editing was deleted.\r\n");
		}
		if (GET_OLC_MOBILE(dsc) && delete_from_proto_list_by_vnum(&GET_OLC_MOBILE(dsc)->proto_script, vnum)) {
			msg_to_char(dsc->character, "A trigger attached to the mobile you're editing was deleted.\r\n");
		}
		if (GET_OLC_ROOM_TEMPLATE(dsc) && delete_from_proto_list_by_vnum(&GET_OLC_ROOM_TEMPLATE(dsc)->proto_script, vnum)) {
			msg_to_char(dsc->character, "A trigger attached to the room template you're editing was deleted.\r\n");
		}
	}
	
	// save index and trigger file now
	save_index(DB_BOOT_TRG);
	save_library_file_for_vnum(DB_BOOT_TRG, vnum);
	
	syslog(SYS_OLC, GET_INVIS_LEV(ch), TRUE, "OLC: %s has deleted trigger %d", GET_NAME(ch), vnum);
	msg_to_char(ch, "Trigger %d deleted.\r\n", vnum);
	
	free_trigger(trig);
}


/**
* Searches for all uses of a crop and displays them.
*
* @param char_data *ch The player.
* @param crop_vnum vnum The crop vnum.
*/
void olc_search_trigger(char_data *ch, trig_vnum vnum) {
	char buf[MAX_STRING_LENGTH];
	trig_data *proto = real_trigger(vnum);
	struct trig_proto_list *trig;
	room_template *rmt, *next_rmt;
	char_data *mob, *next_mob;
	obj_data *obj, *next_obj;
	int size, found;
	bool any;
	
	if (!proto) {
		msg_to_char(ch, "There is no trigger %d.\r\n", vnum);
		return;
	}
	
	found = 0;
	size = snprintf(buf, sizeof(buf), "Occurrences of trigger %d (%s):\r\n", vnum, GET_TRIG_NAME(proto));
	
	// mobs
	HASH_ITER(hh, mobile_table, mob, next_mob) {
		any = FALSE;
		for (trig = mob->proto_script; trig && !any; trig = trig->next) {
			if (trig->vnum == vnum) {
				any = TRUE;
				++found;
				size += snprintf(buf + size, sizeof(buf) - size, "MOB [%5d] %s\r\n", GET_MOB_VNUM(mob), GET_SHORT_DESC(mob));
			}
		}
	}
	
	// objs
	HASH_ITER(hh, object_table, obj, next_obj) {
		any = FALSE;
		for (trig = obj->proto_script; trig && !any; trig = trig->next) {
			if (trig->vnum == vnum) {
				any = TRUE;
				++found;
				size += snprintf(buf + size, sizeof(buf) - size, "OBJ [%5d] %s\r\n", GET_OBJ_VNUM(obj), GET_OBJ_SHORT_DESC(obj));
			}
		}
	}
	
	// room templates
	HASH_ITER(hh, room_template_table, rmt, next_rmt) {
		any = FALSE;
		for (trig = GET_RMT_SCRIPTS(rmt); trig && !any; trig = trig->next) {
			if (trig->vnum == vnum) {
				any = TRUE;
				++found;
				size += snprintf(buf + size, sizeof(buf) - size, "RMT [%5d] %s\r\n", GET_RMT_VNUM(rmt), GET_RMT_TITLE(rmt));
			}
		}
	}
	
	if (found > 0) {
		size += snprintf(buf + size, sizeof(buf) - size, "%d location%s shown\r\n", found, PLURAL(found));
	}
	else {
		size += snprintf(buf + size, sizeof(buf) - size, " none\r\n");
	}
	
	page_string(ch->desc, buf, TRUE);
}


/**
* Function to save a player's changes to a trigger (or a new one).
*
* @param descriptor_data *desc The descriptor who is saving.
*/
void save_olc_trigger(descriptor_data *desc, char *script_text) {
	extern struct cmdlist_element *compile_command_list(char *input);
	void free_varlist(struct trig_var_data *vd);
	
	trig_data *proto, *live_trig, *trig = GET_OLC_TRIGGER(desc);
	trig_vnum vnum = GET_OLC_VNUM(desc);
	struct cmdlist_element *cmd, *next_cmd;
	bool free_text = FALSE;
	UT_hash_handle hh;
	
	// have a place to save it?
	if (!(proto = real_trigger(vnum))) {
		proto = create_trigger_table_entry(vnum);
	}
	
	// free existing commands
	for (cmd = proto->cmdlist; cmd; cmd = next_cmd) { 
		next_cmd = cmd->next;
		if (cmd->cmd)
			free(cmd->cmd);
		free(cmd);
	}

	// free old data on the proto
	if (proto->arglist) {
		free(proto->arglist);
	}
	if (proto->name) {
		free(proto->name);
	}
	
	if (!*script_text) {
		// do not free old script text
		script_text = str_dup("%echo% This trigger commandlist is not complete!\r\n");
		free_text = TRUE;
	}
	
	// Recompile the command list from the new script
	trig->cmdlist = compile_command_list(script_text);
	
	if (free_text) {
		free(script_text);
		script_text = NULL;
	}

	// make the prorotype look like what we have
	hh = proto->hh;	// preserve hash handle
	trig_data_copy(proto, trig);
	proto->hh = hh;
	proto->vnum = vnum;	// ensure correct vnu,

	// go through the mud and replace existing triggers
	for (live_trig = trigger_list; live_trig; live_trig = live_trig->next_in_world) {
		if (GET_TRIG_VNUM(live_trig) == vnum) {
			if (live_trig->arglist) {
				free(live_trig->arglist);
				live_trig->arglist = NULL;
			}
			if (live_trig->name) {
				free(live_trig->name);
				live_trig->name = NULL;
			}

			if (proto->arglist)
				live_trig->arglist = strdup(proto->arglist);
			if (proto->name)
				live_trig->name = strdup(proto->name);

			// anything could have happened so we don't want to keep these
			if (GET_TRIG_WAIT(live_trig)) {
				event_cancel(GET_TRIG_WAIT(live_trig));
				GET_TRIG_WAIT(live_trig)=NULL;
			}
			if (live_trig->var_list) {
				free_varlist(live_trig->var_list);
				live_trig->var_list=NULL;
			}

			live_trig->cmdlist = proto->cmdlist;
			live_trig->curr_state = live_trig->cmdlist;
			live_trig->trigger_type = proto->trigger_type;
			live_trig->attach_type = proto->attach_type;
			live_trig->narg = proto->narg;
			live_trig->data_type = proto->data_type;
			live_trig->depth = 0;
		}
	}
	
	save_library_file_for_vnum(DB_BOOT_TRG, vnum);
}


/**
* Creates a copy of a trigger, or clears a new one, for editing.
* 
* @param struct trig_data *input The trigger to copy, or NULL to make a new one.
* @param char **cmdlist_storage A place to store the command list e.g. &GET_OLC_STORAGE(ch->desc)
* @return struct trig_data* The copied trigger.
*/
struct trig_data *setup_olc_trigger(struct trig_data *input, char **cmdlist_storage) {
	struct cmdlist_element *c;
	struct trig_data *new;
	
	CREATE(*cmdlist_storage, char, MAX_CMD_LENGTH);
	CREATE(new, struct trig_data, 1);
	trig_data_init(new);
	
	if (input) {
		*new = *input;
		
		new->next = NULL;
		new->next_in_world = NULL;
		
		new->name = str_dup(NULLSAFE(input->name));
		new->arglist = input->arglist ? str_dup(input->arglist) : NULL;
		
		// don't copy these things
		new->cmdlist = NULL;
		new->curr_state = NULL;
		new->wait_event = NULL;
		new->var_list = NULL;

		// convert cmdlist to a char string
		c = input->cmdlist;
		strcpy(*cmdlist_storage, "");

		while (c) {
			strcat(*cmdlist_storage, c->cmd);
			strcat(*cmdlist_storage, "\r\n");
			c = c->next;
		}
		// now the cmdlist is something to pass to the text editor
		// it will be converted back to a real cmdlist_element list later
	}
	else {
		new->vnum = NOTHING;

		// Set up some defaults
		new->name = strdup("new trigger");
		new->attach_type = MOB_TRIGGER;
		new->trigger_type = NOBITS;

		// cmdlist will be a large char string until the trigger is saved
		strncpy(*cmdlist_storage, "", MAX_CMD_LENGTH-1);
		new->narg = 100;
	}
	
	// done
	return new;	
}


 //////////////////////////////////////////////////////////////////////////////
//// DISPLAYS ////////////////////////////////////////////////////////////////

/**
* This is the main display for trigger OLC. It displays the user's
* currently-edited trigger.
*
* @param char_data *ch The person who is editing a trigger and will see its display.
*/
void olc_show_trigger(char_data *ch) {
	extern char *show_color_codes(char *string);
	
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	bitvector_t trig_arg_types = compile_argument_types_for_trigger(trig);
	char trgtypes[256];
	
	if (!trig) {
		return;
	}
	
	*buf = '\0';
	
	sprintf(buf + strlen(buf), "[&c%d&0] &c%s&0\r\n", GET_OLC_VNUM(ch->desc), !real_trigger(GET_OLC_VNUM(ch->desc)) ? "new trigger" : GET_TRIG_NAME(real_trigger(GET_OLC_VNUM(ch->desc))));
	sprintf(buf + strlen(buf), "<&yname&0> %s\r\n", NULLSAFE(GET_TRIG_NAME(trig)));
	sprintf(buf + strlen(buf), "<&yattaches&0> %s\r\n", trig_attach_types[trig->attach_type]);
	
	sprintbit(GET_TRIG_TYPE(trig), trig_attach_type_list[trig->attach_type], trgtypes, TRUE);
	sprintf(buf + strlen(buf), "<&ytypes&0> %s\r\n", trgtypes);
	
	if (IS_SET(trig_arg_types, TRIG_ARG_PERCENT)) {
		sprintf(buf + strlen(buf), "<&ypercent&0> %d%%\r\n", trig->narg);
	}
	if (IS_SET(trig_arg_types, TRIG_ARG_PHRASE_OR_WORDLIST)) {
		sprintf(buf + strlen(buf), "<&yargtype&0> %s\r\n", trig_arg_phrase_type[trig->narg]);
	}
	if (IS_SET(trig_arg_types, TRIG_ARG_OBJ_WHERE)) {
		sprintbit(trig->narg, trig_arg_obj_where, buf1, TRUE);
		sprintf(buf + strlen(buf), "<&ylocation&0> %s\r\n", trig->narg ? buf1 : "none");
	}
	if (IS_SET(trig_arg_types, TRIG_ARG_COMMAND | TRIG_ARG_PHRASE_OR_WORDLIST | TRIG_ARG_OBJ_WHERE)) {
		sprintf(buf + strlen(buf), "<&ystring&0> %s\r\n", NULLSAFE(trig->arglist));
	}
	if (IS_SET(trig_arg_types, TRIG_ARG_COST)) {
		sprintf(buf + strlen(buf), "<&ycosts&0> %d other coins\r\n", trig->narg);
	}
	
	sprintf(buf + strlen(buf), "<&ycommands&0>\r\n%s", show_color_codes(NULLSAFE(GET_OLC_STORAGE(ch->desc))));
	
	page_string(ch->desc, buf, TRUE);
}


 //////////////////////////////////////////////////////////////////////////////
//// EDIT MODULES ////////////////////////////////////////////////////////////

OLC_MODULE(tedit_argtype) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	bitvector_t trig_arg_types = compile_argument_types_for_trigger(trig);
	
	if (!IS_SET(trig_arg_types, TRIG_ARG_PHRASE_OR_WORDLIST)) {
		msg_to_char(ch, "You can't set that property on this trigger.\r\n");
	}
	else {
		GET_TRIG_NARG(trig) = olc_process_type(ch, argument, "argument type", "argtype", trig_arg_phrase_type, GET_TRIG_NARG(trig));
	}
}


OLC_MODULE(tedit_attaches) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	int old = trig->attach_type;
	
	trig->attach_type = olc_process_type(ch, argument, "attach type", "attaches", trig_attach_types, trig->attach_type);
	
	if (old != trig->attach_type) {
		GET_TRIG_TYPE(trig) = NOBITS;
	}
}


OLC_MODULE(tedit_commands) {
	if (ch->desc->str) {
		msg_to_char(ch, "You are already editing a string.\r\n");
	}
	else {
		start_string_editor(ch->desc, "trigger commands", &GET_OLC_STORAGE(ch->desc), MAX_CMD_LENGTH);
	}
}


OLC_MODULE(tedit_costs) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	bitvector_t trig_arg_types = compile_argument_types_for_trigger(trig);
	
	if (!IS_SET(trig_arg_types, TRIG_ARG_COST)) {
		msg_to_char(ch, "You can't set that property on this trigger.\r\n");
	}
	else {
		GET_TRIG_NARG(trig) = olc_process_number(ch, argument, "cost", "costs", 0, MAX_INT, GET_TRIG_NARG(trig));
	}
}


OLC_MODULE(tedit_location) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	bitvector_t trig_arg_types = compile_argument_types_for_trigger(trig);
	
	if (!IS_SET(trig_arg_types, TRIG_ARG_OBJ_WHERE)) {
		msg_to_char(ch, "You can't set that property on this trigger.\r\n");
	}
	else {
		GET_TRIG_NARG(trig) = olc_process_flag(ch, argument, "location", "location", trig_arg_obj_where, GET_TRIG_NARG(trig));
	}
}


OLC_MODULE(tedit_name) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	olc_process_string(ch, argument, "name", &GET_TRIG_NAME(trig));
}


OLC_MODULE(tedit_numarg) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	GET_TRIG_NARG(trig) = olc_process_number(ch, argument, "numeric argument", "numarg", -MAX_INT, MAX_INT, GET_TRIG_NARG(trig));
}


OLC_MODULE(tedit_percent) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	bitvector_t trig_arg_types = compile_argument_types_for_trigger(trig);
	
	if (!IS_SET(trig_arg_types, TRIG_ARG_PERCENT)) {
		msg_to_char(ch, "You can't set that property on this trigger.\r\n");
	}
	else {
		GET_TRIG_NARG(trig) = olc_process_number(ch, argument, "percent", "percent", 0, 100, GET_TRIG_NARG(trig));
	}
}


OLC_MODULE(tedit_string) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);
	bitvector_t trig_arg_types = compile_argument_types_for_trigger(trig);
	
	if (!IS_SET(trig_arg_types, TRIG_ARG_COMMAND | TRIG_ARG_PHRASE_OR_WORDLIST | TRIG_ARG_OBJ_WHERE)) {
		msg_to_char(ch, "You can't set that property on this trigger.\r\n");
	}
	else {
		olc_process_string(ch, argument, "string", &GET_TRIG_ARG(trig));
	}
}


OLC_MODULE(tedit_types) {
	trig_data *trig = GET_OLC_TRIGGER(ch->desc);	
	bitvector_t old = GET_TRIG_TYPE(trig);
	
	GET_TRIG_TYPE(trig) = olc_process_flag(ch, argument, "type", "types", trig_attach_type_list[trig->attach_type], GET_TRIG_TYPE(trig));
	
	if (old != GET_TRIG_TYPE(trig)) {
		trig->narg = 0;
		if (trig->arglist) {
			free(trig->arglist);
			trig->arglist = NULL;
		}
	}
}
