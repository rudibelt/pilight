/*
	Copyright (C) 2013 CurlyMo

	This file is part of pilight.

	pilight is free software: you can redistribute it and/or modify it under the
	terms of the GNU General Public License as published by the Free Software
	Foundation, either version 3 of the License, or (at your option) any later
	version.

	pilight is distributed in the hope that it will be useful, but WITHOUT ANY
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <regex.h>
#include <sys/stat.h>
#include <ctype.h>
#include <math.h>

#include "common.h"
#include "json.h"
#include "config.h"
#include "devices.h"
#include "log.h"
#include "gui.h"

static struct gui_elements_t *gui_elements = NULL;

int gui_gc(void) {
	struct gui_elements_t *dtmp;
	struct gui_settings_t *stmp;
	struct gui_values_t *vtmp;

	/* Free devices structure */
	while(gui_elements) {
		dtmp = gui_elements;
		while(dtmp->settings) {
			stmp = dtmp->settings;
			while(stmp->values) {
				vtmp = stmp->values;
				if(vtmp->type == JSON_STRING && vtmp->string_ != NULL) {
					sfree((void *)&vtmp->string_);
				}
				if(vtmp->name) {
					sfree((void *)&vtmp->name);
				}
				stmp->values = stmp->values->next;
				sfree((void *)&vtmp);
			}
			sfree((void *)&stmp->values);
			if(stmp->name) {
				sfree((void *)&stmp->name);
			}
			dtmp->settings = dtmp->settings->next;
			sfree((void *)&stmp);
		}
		sfree((void *)&dtmp->settings);
		sfree((void *)&dtmp->id);
		gui_elements = gui_elements->next;
		sfree((void *)&dtmp);
	}
	sfree((void *)&gui_elements);

	logprintf(LOG_DEBUG, "garbage collected config gui library");

	return EXIT_SUCCESS;
}

struct JsonNode *gui_sync(int level) {
	/* Temporary pointer to the different structure */
	struct gui_elements_t *tmp_gui = NULL;
	struct gui_settings_t *tmp_settings = NULL;
	struct gui_values_t *tmp_values = NULL;
	struct options_t *tmp_options = NULL;
	int i = 0;

	/* Pointers to the newly created JSON object */
	struct JsonNode *jroot = json_mkobject();
	struct JsonNode *jelements = NULL;
	struct JsonNode *joptions = NULL;
	struct JsonNode *jarray = NULL;

	tmp_gui = gui_elements;

	while(tmp_gui) {
		i++;
		jelements = json_mkobject();
		if(level == 0) {
			json_append_member(jelements, "type", json_mknumber(tmp_gui->device->protocols->listener->devtype, 0));
			json_append_member(jelements, "order", json_mknumber(i, 0));
		}

		tmp_settings = tmp_gui->settings;
		while(tmp_settings) {
			tmp_values = tmp_settings->values;
			if(strcmp(tmp_settings->name, "group") == 0 || strcmp(tmp_settings->name, "media") == 0) {
				if(!(jarray = json_find_member(jelements, tmp_settings->name))) {
					jarray = json_mkarray();
					json_append_member(jelements, tmp_settings->name, jarray);
				}
				while(tmp_values) {
					if(tmp_values->type == JSON_NUMBER) {
						json_append_element(jarray, json_mknumber(tmp_values->number_, tmp_values->decimals));
					} else if(tmp_values->type == JSON_STRING) {
						json_append_element(jarray, json_mkstring(tmp_values->string_));
					}
					tmp_values = tmp_values->next;
				}
			} else if(!tmp_values->next) {
				if(tmp_values->type == JSON_NUMBER) {
					json_append_member(jelements, tmp_settings->name, json_mknumber(tmp_values->number_, tmp_values->decimals));
				} else if(tmp_values->type == JSON_STRING) {
					json_append_member(jelements, tmp_settings->name, json_mkstring(tmp_values->string_));
				}
			} else {
				joptions = json_mkarray();
				while(tmp_values) {
					if(tmp_values->type == JSON_NUMBER) {
						json_append_element(joptions, json_mknumber(tmp_values->number_, tmp_values->decimals));
					} else if(tmp_values->type == JSON_STRING) {
						json_append_element(joptions, json_mkstring(tmp_values->string_));
					}
					tmp_values = tmp_values->next;
				}
				json_append_member(jelements, tmp_settings->name, joptions);
			}
			tmp_settings = tmp_settings->next;
		}

		if(level == 0 && !(jarray = json_find_member(jelements, "media"))) {
			jarray = json_mkarray();
			json_append_element(jarray, json_mkstring("all"));
			json_append_member(jelements, "media", jarray);
		}

		struct protocols_t *tmp_protocols = tmp_gui->device->protocols;
		while(tmp_protocols) {
			tmp_options = tmp_protocols->listener->options;
			if(tmp_options) {
				while(tmp_options) {
					if(level == 0 && (tmp_options->conftype == GUI_SETTING)
					&& json_find_member(jelements, tmp_options->name) == NULL) {
						if(tmp_options->vartype == JSON_NUMBER) {
							json_append_member(jelements, tmp_options->name, json_mknumber((int)(intptr_t)tmp_options->def, 0));
						} else if(tmp_options->vartype == JSON_STRING) {
							json_append_member(jelements, tmp_options->name, json_mkstring((char *)tmp_options->def));
						}
					}
					tmp_options = tmp_options->next;
				}
			}
			tmp_protocols = tmp_protocols->next;
		}
		json_append_member(jroot, tmp_gui->id, jelements);
		tmp_gui = tmp_gui->next;
	}

	return jroot;
}

/* Save the gui settings to the element struct */
static void gui_save_setting(int i, JsonNode *jsetting, struct gui_elements_t *element) {
	/* Struct to store the values */
	struct gui_values_t *vnode = NULL;
	struct gui_settings_t *snode = NULL;
	struct gui_settings_t *tmp_settings = NULL;
	struct gui_values_t *tmp_values = NULL;
	/* Temporary JSON pointer */
	struct JsonNode *jtmp;

	/* Variable holder for casting settings */
	char *stmp = NULL;

	/* If the JSON tag is an array, then it should be a values or id array */
	if(jsetting->tag == JSON_ARRAY) {
		if(strcmp(jsetting->key, "group") == 0 || strcmp(jsetting->key, "media") == 0) {
			/* Loop through the values of this values array */
			jtmp = json_first_child(jsetting);
			snode = malloc(sizeof(struct gui_settings_t));
			if(!snode) {
				logprintf(LOG_ERR, "out of memory");
				exit(EXIT_FAILURE);
			}
			snode->name = malloc(strlen(jsetting->key)+1);
			if(!snode->name) {
				logprintf(LOG_ERR, "out of memory");
				exit(EXIT_FAILURE);
			}
			strcpy(snode->name, jsetting->key);
			snode->values = NULL;
			snode->next = NULL;
			while(jtmp) {
				vnode = malloc(sizeof(struct gui_values_t));
				if(!vnode) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				vnode->name = NULL;
				vnode->next = NULL;
				if(jtmp->tag == JSON_STRING) {
					vnode->string_ = malloc(strlen(jtmp->string_)+1);
					if(!vnode->string_) {
						logprintf(LOG_ERR, "out of memory");
						exit(EXIT_FAILURE);
					}
					strcpy(vnode->string_, jtmp->string_);
					vnode->type = JSON_STRING;
				} else if(jtmp->tag == JSON_NUMBER) {
					vnode->number_ = jtmp->number_;
					vnode->decimals = jtmp->decimals_;
					vnode->type = JSON_NUMBER;
				}
				if(jtmp->tag == JSON_NUMBER || jtmp->tag == JSON_STRING) {
					tmp_values = snode->values;
					if(tmp_values) {
						while(tmp_values->next != NULL) {
							tmp_values = tmp_values->next;
						}
						tmp_values->next = vnode;
					} else {
						vnode->next = snode->values;
						snode->values = vnode;
					}
				}
				jtmp = jtmp->next;
			}
			tmp_settings = element->settings;
			if(tmp_settings) {
				while(tmp_settings->next != NULL) {
					tmp_settings = tmp_settings->next;
				}
				tmp_settings->next = snode;
			} else {
				snode->next = element->settings;
				element->settings = snode;
			}
		}
	} else if(jsetting->tag == JSON_OBJECT) {
		snode = malloc(sizeof(struct gui_settings_t));
		if(!snode) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		snode->name = malloc(strlen(jsetting->key)+1);
		if(!snode->name) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		strcpy(snode->name, jsetting->key);
		snode->values = NULL;
		snode->next = NULL;

		jtmp = json_first_child(jsetting);
		while(jtmp) {
			if(jtmp->tag == JSON_STRING) {
				vnode = malloc(sizeof(struct gui_values_t));
				if(!vnode) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				vnode->name = malloc(strlen(jtmp->key)+1);
				if(!vnode->name) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				strcpy(vnode->name, jtmp->key);
				vnode->string_ = malloc(strlen(jtmp->string_)+1);
				if(!vnode->string_) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				strcpy(vnode->string_, jtmp->string_);
				vnode->type = JSON_STRING;
				vnode->next = NULL;
			} else if(jtmp->tag == JSON_NUMBER) {
				vnode = malloc(sizeof(struct gui_values_t));
				if(!vnode) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				vnode->name = malloc(strlen(jtmp->key)+1);
				if(!vnode->name) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				strcpy(vnode->name, jtmp->key);
				vnode->number_ = jtmp->number_;
				vnode->decimals = jtmp->decimals_;
				vnode->type = JSON_NUMBER;
				vnode->next = NULL;
			}
			if(jtmp->tag == JSON_NUMBER || jtmp->tag == JSON_STRING) {
				tmp_values = snode->values;
				if(tmp_values) {
					while(tmp_values->next != NULL) {
						tmp_values = tmp_values->next;
					}
					tmp_values->next = vnode;
				} else {
					vnode->next = snode->values;
					snode->values = vnode;
				}
			}
			jtmp = jtmp->next;
		}

		tmp_settings = element->settings;
		if(tmp_settings) {
			while(tmp_settings->next != NULL) {
				tmp_settings = tmp_settings->next;
			}
			tmp_settings->next = snode;
		} else {
			snode->next = element->settings;
			element->settings = snode;
		}

	} else {
		/* New element settings node */
		snode = malloc(sizeof(struct gui_settings_t));
		if(!snode) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		snode->name = malloc(strlen(jsetting->key)+1);
		if(!snode->name) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		strcpy(snode->name, jsetting->key);
		snode->values = NULL;
		snode->next = NULL;

		vnode = malloc(sizeof(struct gui_values_t));
		if(!vnode) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		int valid = 0;
		/* Cast and store the new value */
		if(jsetting->tag == JSON_STRING && json_find_string(jsetting->parent, jsetting->key, &stmp) == 0) {
			vnode->string_ = malloc(strlen(stmp)+1);
			if(!vnode->string_) {
				logprintf(LOG_ERR, "out of memory");
				exit(EXIT_FAILURE);
			}
			vnode->name = NULL;
			strcpy(vnode->string_, stmp);
			vnode->type = JSON_STRING;
			valid = 1;
		} else if(jsetting->tag == JSON_NUMBER &&
		         (jtmp = json_find_member(jsetting->parent, jsetting->key)) != NULL &&
				  jtmp->tag == JSON_NUMBER) {
			vnode->name = NULL;
			vnode->number_ = jtmp->number_;
			vnode->decimals = jtmp->decimals_;
			vnode->type = JSON_NUMBER;
			valid = 1;
		}
		if(valid) {
			tmp_values = snode->values;
			if(tmp_values) {
				while(tmp_values->next != NULL) {
					tmp_values = tmp_values->next;
				}
				tmp_values->next = vnode;
			} else {
				vnode->next = snode->values;
				snode->values = vnode;
			}
		}

		tmp_settings = element->settings;
		if(tmp_settings) {
			while(tmp_settings->next != NULL) {
				tmp_settings = tmp_settings->next;
			}
			tmp_settings->next = snode;
		} else {
			snode->next = element->settings;
			element->settings = snode;
		}
	}
}

int gui_parse_elements(struct JsonNode *root, struct gui_elements_t *parent, int i) {
	struct JsonNode *jsettings = NULL;
	int nrgroup = 0, nrmedia = 0, nrname = 0, nrorder = 0;
	int valid_setting = 0;
	int have_error = 0;

	jsettings = json_first_child(root);
	while(jsettings) {
		if(strcmp(jsettings->key, "group") == 0) {
			nrgroup++;
		} else if(strcmp(jsettings->key, "media") == 0) {
			nrmedia++;
		} else if(strcmp(jsettings->key, "name") == 0) {
			nrname++;
		} else if(strcmp(jsettings->key, "name") == 0) {
			nrorder++;
		}
		if(nrmedia > 1 || nrgroup > 1 || nrname > 1 || nrorder > 1) {
			logprintf(LOG_ERR, "config gui element #%d \"%s\" of \"%s\", duplicate", i, jsettings->key, root->key);
			have_error = 1;
			goto clear;
		}
		if(strcmp(jsettings->key, "group") == 0) {
			if(jsettings->tag != JSON_ARRAY) {
				logprintf(LOG_ERR, "config gui element #%d \"%s\" of \"%s\", invalid", i, jsettings->key, root->key);
				have_error = 1;
				goto clear;
			} else {
				gui_save_setting(i, jsettings, parent);
			}
		} else if(strcmp(jsettings->key, "name") == 0) {
			if(jsettings->tag != JSON_STRING) {
				logprintf(LOG_ERR, "config gui element #%d \"%s\" of \"%s\", invalid", i, jsettings->key, root->key);
				have_error = 1;
				goto clear;
			} else {
				gui_save_setting(i, jsettings, parent);
			}
		} else if(strcmp(jsettings->key, "media") == 0) {
			if(jsettings->tag != JSON_ARRAY) {
				logprintf(LOG_ERR, "config gui element #%d \"%s\" of \"%s\", invalid", i, jsettings->key, root->key);
				have_error = 1;
				goto clear;
			} else {
				struct JsonNode *jvalues = json_first_child(jsettings);
				int nrvalues = 0;
				int hasall = 0;
				while(jvalues) {
					if(jvalues->tag == JSON_STRING) {
						if(strcmp(jvalues->string_, "web") != 0 &&
							strcmp(jvalues->string_, "mobile") != 0 &&
							strcmp(jvalues->string_, "desktop") != 0 &&
							strcmp(jvalues->string_, "all") != 0) {
							logprintf(LOG_ERR, "config gui element #%d \"media\" can only contain \"web\", \"desktop\", \"mobile\", or \"all\"", i, jsettings->key, root->key);
							have_error = 1;
							goto clear;
							break;
						} else {
							nrvalues++;
						}
						if(strcmp(jvalues->string_, "all") == 0) {
							hasall = 1;
						}
					} else {
						logprintf(LOG_ERR, "config gui element #%d \"media\" of \"%s\", invalid", i, root->key);
						have_error = 1;
						goto clear;
						break;
					}
					if(hasall == 1 && nrvalues > 1) {
						logprintf(LOG_ERR, "config gui element #%d \"media\" value \"all\" cannot be combined with other values", i, jsettings->key, root->key);
						have_error = 1;
						goto clear;
					}
					jvalues = jvalues->next;
				}
				gui_save_setting(i, jsettings, parent);
			}
		} else if(!((strcmp(jsettings->key, "name") == 0 && jsettings->tag == JSON_STRING)
				  || (strcmp(jsettings->key, "type") == 0 && jsettings->tag == JSON_NUMBER)
				  || (strcmp(jsettings->key, "order") == 0 && jsettings->tag == JSON_NUMBER))) {
			valid_setting = 0;
			/* Check if the optional settings are valid
			   for the protocol(s) */
			struct protocols_t *proto = parent->device->protocols;
			while(proto) {
				struct options_t *options = proto->listener->options;
				while(options) {
					if(options->conftype == GUI_SETTING &&
					   options->vartype == jsettings->tag &&
					   strcmp(jsettings->key, options->name) == 0) {
						valid_setting = 1;
						break;
					}
					options = options->next;
				}
				proto = proto->next;
			}
			if(valid_setting == 1) {
				gui_save_setting(i, jsettings, parent);
			} else {
				logprintf(LOG_ERR, "config gui element #%d \"%s\" of \"%s\", invalid", i, jsettings->key, root->key);
				have_error = 1;
				goto clear;
			}
		}

		jsettings = jsettings->next;
	}

clear:
	return have_error;
}

int gui_read(struct JsonNode *root) {
	struct gui_elements_t *dnode = NULL;
	struct gui_elements_t *tmp_gui = NULL;
	struct JsonNode *jelements = NULL;

	int i = 0, have_error = 0;

	jelements = json_first_child(root);
	while(jelements) {
		i++;
		if(jelements->tag != JSON_OBJECT) {
			logprintf(LOG_ERR, "config gui element #%d \"%s\", invalid field(s)", i, jelements->key);
			have_error = 1;
			goto clear;
		} else {
			/* Check for duplicate fields */
			tmp_gui = gui_elements;
			while(tmp_gui) {
				if(strcmp(tmp_gui->id, jelements->key) == 0) {
					logprintf(LOG_ERR, "config gui element #%d \"%s\", duplicate", i, jelements->key);
					have_error = 1;
				}
				tmp_gui = tmp_gui->next;
			}

			dnode = malloc(sizeof(struct gui_elements_t));
			if(!dnode) {
				logprintf(LOG_ERR, "out of memory");
				exit(EXIT_FAILURE);
			}
			dnode->settings = NULL;
			dnode->next = NULL;
			dnode->device = NULL;

			if(devices_get(jelements->key, &dnode->device) != 0) {
				logprintf(LOG_ERR, "config gui element #%d \"%s\", device not configured", i, jelements->key);
				have_error = 1;
				sfree((void *)&dnode);
				goto clear;
			}
			dnode->id = malloc(strlen(jelements->key)+1);
			if(!dnode->id ) {
				logprintf(LOG_ERR, "out of memory");
				exit(EXIT_FAILURE);
			}
			strcpy(dnode->id, jelements->key);

			if(!have_error && gui_parse_elements(jelements, dnode, i) != 0) {
				have_error = 1;
			}

			tmp_gui = gui_elements;
			if(tmp_gui) {
				while(tmp_gui->next != NULL) {
					tmp_gui = tmp_gui->next;
				}
				tmp_gui->next = dnode;
			} else {
				dnode->next = gui_elements;
				gui_elements = dnode;
			}

			if(have_error) {
				goto clear;
			}
		}
		jelements = jelements->next;
	}
clear:
	return have_error;
}

void gui_init(void) {
	/* Request hardware json object in main configuration */
	config_register(&config_gui, "gui");
	config_gui->readorder = 2;
	config_gui->writeorder = 2;
	config_gui->parse=&gui_read;
	config_gui->sync=&gui_sync;
	config_gui->gc=&gui_gc;
}
