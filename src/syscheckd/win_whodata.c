/*
 * Copyright (C) 2015-2019, Wazuh Inc.
 * June 13, 2018.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */
#include "shared.h"
#include "hash_op.h"
#include "syscheck.h"
#include "syscheck_op.h"

#ifdef WIN_WHODATA

#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winevt.h>

#define WLIST_ALERT_THRESHOLD 80 // 80%
#define WLIST_REMOVE_MAX 10 // 10%
#define WLIST_MAX_SIZE OS_SIZE_1024
#define WPOL_BACKUP_COMMAND "auditpol /backup /file:\"%s\""
#define WPOL_RESTORE_COMMAND "auditpol /restore /file:\"%s\""
#define WPOL_BACKUP_FILE "tmp\\backup-policies"
#define WPOL_NEW_FILE "tmp\\new-policies"
#define modify_criteria (FILE_WRITE_DATA | WRITE_DAC | FILE_WRITE_ATTRIBUTES)
#define criteria (DELETE | modify_criteria)

// Variables whodata
static PSID everyone_sid = NULL;
static size_t ev_sid_size = 0;
static unsigned short inherit_flag = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE; //SUB_CONTAINERS_AND_OBJECTS_INHERIT
static EVT_HANDLE context;
static const wchar_t* event_fields[] = {
    L"Event/System/EventID",
    L"Event/EventData/Data[@Name='SubjectUserName']",
    L"Event/EventData/Data[@Name='ObjectName']",
    L"Event/EventData/Data[@Name='ProcessName']",
    L"Event/EventData/Data[@Name='ProcessId']",
    L"Event/EventData/Data[@Name='HandleId']",
    L"Event/EventData/Data[@Name='AccessMask']",
    L"Event/EventData/Data[@Name='SubjectUserSid']",
    L"Event/System/TimeCreated/@SystemTime"
};
static unsigned int fields_number = sizeof(event_fields) / sizeof(LPWSTR);
static const unsigned __int64 AUDIT_SUCCESS = 0x20000000000000;
static LPCTSTR priv = "SeSecurityPrivilege";
static int restore_policies = 0;

// Whodata function headers
void restore_sacls();
int set_privilege(HANDLE hdle, LPCTSTR privilege, int enable);
int is_valid_sacl(PACL sacl, int is_file);
unsigned long WINAPI whodata_callback(EVT_SUBSCRIBE_NOTIFY_ACTION action, __attribute__((unused)) void *_void, EVT_HANDLE event);
char *guid_to_string(GUID *guid);
int set_policies();
void set_subscription_query(wchar_t *query);
extern int wm_exec(char *command, char **output, int *exitcode, int secs, const char * add_path);
int restore_audit_policies();
void audit_restore();
int check_object_sacl(char *obj, int is_file);
int whodata_hash_add(OSHash *table, char *id, void *data, char *tag);
void notify_SACL_change(char *dir);

// Whodata list operations
whodata_event_node *whodata_list_add(char *id);
void whodata_list_remove(whodata_event_node *node);
void whodata_list_set_values();
void whodata_list_remove_multiple(size_t quantity);
void send_whodata_del(whodata_evt *w_evt);
int get_file_time(unsigned long long file_time_val, SYSTEMTIME *system_time);
int compare_timestamp(SYSTEMTIME *t1, SYSTEMTIME *t2);
void free_win_whodata_evt(whodata_evt *evt);

char *guid_to_string(GUID *guid) {
    char *string_guid;
    os_calloc(40, sizeof(char *), string_guid);

    snprintf(string_guid, 40, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
    guid->Data1,
    guid->Data2,
    guid->Data3,
    guid->Data4[0], guid->Data4[1],
    guid->Data4[2], guid->Data4[3],guid->Data4[4], guid->Data4[5],guid->Data4[6], guid->Data4[7]);

    return string_guid;
}

int set_winsacl(const char *dir, int position) {
	DWORD result = 0;
	PACL old_sacl = NULL, new_sacl = NULL;
	PSECURITY_DESCRIPTOR security_descriptor = NULL;
    SYSTEM_AUDIT_ACE *ace = NULL;
    PVOID entry_access_it = NULL;
	HANDLE hdle;
    unsigned int i;
    ACL_SIZE_INFORMATION old_sacl_info;
    unsigned long new_sacl_size;
    int retval = 1;
    int privilege_enabled = 0;

    mdebug2(FIM_SACL_CONFIGURE, dir);

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hdle)) {
		merror(FIM_ERROR_SACL_OPENPROCESSTOKEN, GetLastError());
		return 1;
	}

	if (set_privilege(hdle, priv, TRUE)) {
		merror(FIM_ERROR_SACL_ELEVATE_PRIVILEGE, GetLastError());
		return 1;
	}

    privilege_enabled = 1;

	if (result = GetNamedSecurityInfo(dir, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, &old_sacl, &security_descriptor), result != ERROR_SUCCESS) {
		merror(FIM_ERROR_SACL_GETSECURITYINFO, result);
        goto end;
	}

    ZeroMemory(&old_sacl_info, sizeof(ACL_SIZE_INFORMATION));

    // Check if the sacl has what the whodata scanner needs
    switch(is_valid_sacl(old_sacl, (syscheck.wdata.dirs_status[position].object_type == WD_STATUS_FILE_TYPE) ? 1 : 0)) {
        case 0:
            mdebug1(FIM_SACL_CHECK_CONFIGURE, dir);
            syscheck.wdata.dirs_status[position].status |= WD_IGNORE_REST;

            // Get SACL size
            if (!GetAclInformation(old_sacl, (LPVOID)&old_sacl_info, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation)) {
                merror(FIM_ERROR_SACL_GETSIZE, dir);
                goto end;
            }
        break;
        case 1:
            mdebug1(FIM_SCAL_NOCONFIGURE, dir);
            retval = 0;
            goto end;
        case 2:
            // Empty SACL
            syscheck.wdata.dirs_status[position].status |= WD_IGNORE_REST;
            old_sacl_info.AclBytesInUse = sizeof(ACL);
            break;
    }
    if (!ev_sid_size) {
        ev_sid_size = GetLengthSid(everyone_sid);
    }

    // Set the new ACL size
    new_sacl_size = old_sacl_info.AclBytesInUse + sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(unsigned long);

    if (new_sacl = (PACL)win_alloc(new_sacl_size), !new_sacl) {
        merror(FIM_ERROR_SACL_NOMEMORY, dir);
        goto end;
    }

    if (!InitializeAcl(new_sacl, new_sacl_size, ACL_REVISION)) {
        merror(FIM_ERROR_SACL_CREATE, dir);
        goto end;
    }

    // If SACL is present, copy it to a new SACL
    if (old_sacl) {
        if (old_sacl_info.AceCount) {
            for (i = 0; i < old_sacl_info.AceCount; i++) {
               if (!GetAce(old_sacl, i, &entry_access_it)) {
                   merror(FIM_ERROR_SACL_ACE_GET, i, dir);
                   goto end;
               }

               if (!AddAce(new_sacl, ACL_REVISION, MAXDWORD, entry_access_it, ((PACE_HEADER)entry_access_it)->AceSize)) {
                   merror(FIM_ERROR_SACL_ACE_CPY, i, dir);
                   goto end;
               }
           }
        }
    }
    // Build the new ACE
    if (ace = (SYSTEM_AUDIT_ACE *)win_alloc(sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(DWORD)), !ace) {
        merror(FIM_ERROR_SACL_ACE_NOMEMORY, dir);
        goto end;
    }

    ace->Header.AceType  = SYSTEM_AUDIT_ACE_TYPE;
    ace->Header.AceFlags = inherit_flag | SUCCESSFUL_ACCESS_ACE_FLAG;
    ace->Header.AceSize  = LOWORD(sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(DWORD));
    ace->Mask            = criteria;
    if (!CopySid(ev_sid_size, &ace->SidStart, everyone_sid)) {
        goto end;
    }

    // Add the new ACE
    if (!AddAce(new_sacl, ACL_REVISION, 0, (LPVOID)ace, ace->Header.AceSize)) {
		merror(FIM_ERROR_SACL_ACE_ADD, dir);
		goto end;
	}

    // Set a new ACL for the security descriptor
    if (result = SetNamedSecurityInfo((char *) dir, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, new_sacl), result != ERROR_SUCCESS) {
        merror(FIM_ERROR_SACL_SETSECURITYINFO, result);
        goto end;
    }

	retval = 0;
end:
    if (privilege_enabled) {
        // Disable the privilege
        if (set_privilege(hdle, priv, FALSE)) {
            merror(FIM_ERROR_SACL_SET_PRIVILEGE, GetLastError());
        }
    }

    if (hdle) {
        CloseHandle(hdle);
    }

    if (security_descriptor) {
        LocalFree((HLOCAL)security_descriptor);
    }

    if (old_sacl) {
        LocalFree((HLOCAL)old_sacl);
    }

    if (new_sacl) {
        LocalFree((HLOCAL)new_sacl);
    }
    if (ace) {
        LocalFree((HLOCAL)ace);
    }
    return retval;
}

int is_valid_sacl(PACL sacl, int is_file) {
    int i;
    ACCESS_ALLOWED_ACE *ace;
    SID_IDENTIFIER_AUTHORITY world_auth = {SECURITY_WORLD_SID_AUTHORITY};

    if (!everyone_sid) {
        if (!AllocateAndInitializeSid(&world_auth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone_sid)) {
            merror(FIM_ERROR_WHODATA_GET_SID, GetLastError());
            return 0;
        }
    }

    if (!sacl) {
        mdebug2(FIM_SACL_NOT_FOUND);
        return 2;
    }

    for (i = 0; i < sacl->AceCount; i++) {
        if (!GetAce(sacl, i, (LPVOID*)&ace)) {
            merror(FIM_ERROR_WHODATA_GET_ACE, GetLastError());
            return 0;
        }

        if ((is_file || (ace->Header.AceFlags & inherit_flag)) && // Check folder and subfolders
            (ace->Header.AceFlags & SUCCESSFUL_ACCESS_ACE_FLAG) && // Check successful attemp
            ((ace->Mask & (criteria)) == criteria) && // Check write, delete, change_permissions and change_attributes permission
            (EqualSid((PSID)&ace->SidStart, everyone_sid))) { // Check everyone user
            return 1;
        }
    }
    return 0;
}

int set_privilege(HANDLE hdle, LPCTSTR privilege, int enable) {
	TOKEN_PRIVILEGES tp;
	LUID pr_uid;

	// Get the privilege UID
	if (!LookupPrivilegeValue(NULL, privilege, &pr_uid)) {
		merror(FIM_ERROR_SACL_FIND_PRIVILEGE, privilege, GetLastError());
		return 1;
	}

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = pr_uid;

	if (enable) {
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	} else {
		tp.Privileges[0].Attributes = 0;
	}

    // Set the privilege to the process
	if (!AdjustTokenPrivileges(hdle, 0, &tp, sizeof(TOKEN_PRIVILEGES), (PTOKEN_PRIVILEGES)NULL, (PDWORD)NULL)) {
		merror(FIM_ERROR_WHODATA_TOKENPRIVILEGES, GetLastError());
		return 1;
	}

    if (enable) {
        mdebug2(FIM_ELEVATE_PRIVILEGE, privilege);
    } else {
        mdebug2(FIM_REDUCE_PRIVILEGE, privilege);
    }

	return 0;
}

int run_whodata_scan() {
    wchar_t query[OS_MAXSTR];
    int result;

    // Set the signal handler to restore the policies
    atexit(audit_restore);
    // Set the system audit policies
    if (result = set_policies(), result) {
        if (result == 2) {
            mwarn("Audit policies could not be auto-configured due to the Windows version. Check if they are correct for whodata mode.");
        } else {
            mwarn("Local audit policies could not be configured.");
            return 1;
        }
    }
    // Select the interesting fields
    if (context = EvtCreateRenderContext(fields_number, event_fields, EvtRenderContextValues), !context) {
        merror(FIM_ERROR_WHODATA_CONTEXT, GetLastError());
        return 1;
    }

    set_subscription_query(query);

    // Set the whodata callback
    if (!EvtSubscribe(NULL, NULL, L"Security", query,
            NULL, NULL, (EVT_SUBSCRIBE_CALLBACK)whodata_callback, EvtSubscribeToFutureEvents)) {
        merror(FIM_ERROR_WHODATA_EVENTCHANNEL);
        return 1;
    }
    return 0;
}

void audit_restore() {
    restore_sacls();
    if (restore_policies) {
        restore_audit_policies();
    }
}

/* Removes added security audit policies */
void restore_sacls() {
    int i;
    PACL sacl_it;
    HANDLE hdle = NULL;
    HANDLE c_process = NULL;
    LPCTSTR priv = "SeSecurityPrivilege";
    DWORD result = 0;
    PSECURITY_DESCRIPTOR security_descriptor = NULL;
    int privilege_enabled = 0;

    c_process = GetCurrentProcess();
    if (!OpenProcessToken(c_process, TOKEN_ADJUST_PRIVILEGES, &hdle)) {
        merror(FIM_ERROR_SACL_OPENPROCESSTOKEN, GetLastError());
        goto end;
    }

    if (set_privilege(hdle, priv, TRUE)) {
        merror(FIM_ERROR_SACL_ELEVATE_PRIVILEGE, GetLastError());
        goto end;
    }

    privilege_enabled = 1;

    for (i = 0; syscheck.dir[i] != NULL; i++) {
        if (syscheck.wdata.dirs_status[i].status & WD_IGNORE_REST) {
            sacl_it = NULL;
            if (result = GetNamedSecurityInfo(syscheck.dir[i], SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, &sacl_it, &security_descriptor), result != ERROR_SUCCESS) {
                merror(FIM_ERROR_SACL_GETSECURITYINFO, result);
                break;
            }

            // The ACE we added is in position 0
            if (!DeleteAce(sacl_it, 0)) {
                merror(FIM_ERROR_SACL_ACE_DELETE, GetLastError());
                break;
            }

            // Set the SACL
            if (result = SetNamedSecurityInfo((char *) syscheck.dir[i], SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, sacl_it), result != ERROR_SUCCESS) {
                merror(FIM_ERROR_SACL_SETSECURITYINFO, result);
                break;
            }

            if (sacl_it) {
                LocalFree((HLOCAL)sacl_it);
            }

            if (security_descriptor) {
                LocalFree((HLOCAL)security_descriptor);
            }
            mdebug1(FIM_SACL_RESTORED, syscheck.dir[i]);
        }
    }

end:
    if (privilege_enabled) {
        // Disable the privilege
        if (set_privilege(hdle, priv, FALSE)) {
            merror(FIM_ERROR_SACL_SET_PRIVILEGE, GetLastError());
        }
    }

    if (hdle) {
        CloseHandle(hdle);
    }
    if (c_process) {
        CloseHandle(c_process);
    }
}

int restore_audit_policies() {
    char command[OS_SIZE_1024];
    int result_code;
    char *output;
    snprintf(command, OS_SIZE_1024, WPOL_RESTORE_COMMAND, WPOL_BACKUP_FILE);

    if (IsFile(WPOL_BACKUP_FILE)) {
        merror(FIM_ERROR_WHODATA_RESTORE_POLICIES);
        return 1;
    }
    // Get the current policies
    if (wm_exec(command, &output, &result_code, 5, NULL), result_code) {
        merror(FIM_ERROR_WHODATA_AUDITPOL, output);
        return 1;
    }

    return 0;
}

unsigned long WINAPI whodata_callback(EVT_SUBSCRIBE_NOTIFY_ACTION action, __attribute__((unused)) void *_void, EVT_HANDLE event) {
    unsigned int retval = 1;
    int result;
    unsigned long p_count = 0;
    unsigned long used_size;
    PEVT_VARIANT buffer = NULL;
    whodata_evt *w_evt;
    short event_id;
    char *user_name = NULL;
    char *path = NULL;
    char *process_name = NULL;
    unsigned __int64 process_id;
    unsigned __int64 handle_id;
    char *user_id = NULL;
    char is_directory;
    unsigned int mask;
    int position;
    whodata_directory *w_dir;
    SYSTEMTIME system_time;
    syscheck_node *s_node;

    if (action == EvtSubscribeActionDeliver) {
        char hash_id[21];

        // Extract the necessary memory size
        EvtRender(context, event, EvtRenderEventValues, 0, NULL, &used_size, &p_count);
        // We may be taking more memory than we need to
		buffer = (PEVT_VARIANT)malloc(used_size);

        if (!EvtRender(context, event, EvtRenderEventValues, used_size, buffer, &used_size, &p_count)) {
			merror(FIM_ERROR_WHODATA_RENDER_EVENT, GetLastError());
            goto clean;
		}

        if (fields_number != p_count) {
			merror(FIM_ERROR_WHODATA_RENDER_PARAM);
            goto clean;
        }

        if (buffer[0].Type != EvtVarTypeUInt16) {
            merror(FIM_WHODATA_PARAMETER, buffer[0].Type, "event_id");
            goto clean;
        }
        event_id = buffer[0].Int16Val;

        // Check types
        if (buffer[2].Type != EvtVarTypeString) {
            if (event_id == 4658 || event_id == 4660) {
                path = NULL;
            } else {
                merror(FIM_WHODATA_PARAMETER, buffer[2].Type, "path");
                goto clean;
            }
        }  else {
            if (path = convert_windows_string(buffer[2].XmlVal), !path) {
                goto clean;
            }
            str_lowercase(path);
            if (OSHash_Get_ex(syscheck.wdata.ignored_paths, path)) {
                // The file has been marked as ignored
                mdebug2(FIM_WHODATA_IGNORE, path);
                goto clean;
            }
        }

        if (buffer[1].Type != EvtVarTypeString) {
            mwarn(FIM_WHODATA_PARAMETER, buffer[1].Type, "user_name");
            user_name = NULL;
        } else {
            user_name = convert_windows_string(buffer[1].XmlVal);
        }

        if (buffer[3].Type != EvtVarTypeString) {
            mwarn(FIM_WHODATA_PARAMETER, buffer[3].Type, "process_name");
            process_name = NULL;
        } else {
            process_name = convert_windows_string(buffer[3].XmlVal);
        }

        // In 32-bit Windows we find EvtVarTypeSizeT
        if (buffer[4].Type != EvtVarTypeHexInt64) {
            if (buffer[4].Type == EvtVarTypeSizeT) {
                process_id = (unsigned __int64) buffer[4].SizeTVal;
            } else if (buffer[4].Type == EvtVarTypeHexInt32) {
                process_id = (unsigned __int64) buffer[4].UInt32Val;
            } else {
                mwarn(FIM_WHODATA_PARAMETER, buffer[4].Type, "process_id");
                process_id = 0;
            }
        } else {
            process_id = buffer[4].UInt64Val;
        }

        // In 32-bit Windows we find EvtVarTypeSizeT or EvtVarTypeHexInt32
        if (buffer[5].Type != EvtVarTypeHexInt64) {
            if (buffer[5].Type == EvtVarTypeSizeT) {
                handle_id = (unsigned __int64) buffer[5].SizeTVal;
            } else if (buffer[5].Type == EvtVarTypeHexInt32) {
                handle_id = (unsigned __int64) buffer[5].UInt32Val;
            } else {
                merror(FIM_WHODATA_PARAMETER, buffer[5].Type, "handle_id");
                goto clean;
            }
        } else {
            handle_id = buffer[5].UInt64Val;
        }

        if (buffer[6].Type != EvtVarTypeHexInt32) {
            if (event_id == 4658 || event_id == 4660) {
                mask = 0;
            } else {
                merror(FIM_WHODATA_PARAMETER, buffer[6].Type, "mask");
                goto clean;
            }
        } else {
            mask = buffer[6].UInt32Val;
        }

        if (buffer[7].Type != EvtVarTypeSid) {
            mwarn(FIM_WHODATA_PARAMETER, buffer[7].Type, "user_id");
            user_id = NULL;
        } else if (!ConvertSidToStringSid(buffer[7].SidVal, &user_id)) {
            mdebug1(FIM_WHODATA_INVALID_UID, user_name);
            goto clean;
        }
        snprintf(hash_id, 21, "%llu", handle_id);
        switch(event_id) {
            // Open fd
            case 4656:
                is_directory = 0;
                position = -1;

                if (!path) {
                    goto clean;
                }
                // Check if it is a known file
                if (s_node = OSHash_Get_ex(syscheck.fp, path), !s_node) {
                    // Check if it is not a directory
                    if (strchr(path, ':') && check_path_type(path) == 1) {
                        if (position = find_dir_pos(path, 1, CHECK_WHODATA, 1), position < 0) {
                            // Discard the file if its monitoring has not been activated
                            mdebug2(FIM_WHODATA_NOT_ACTIVE, path);
                            whodata_hash_add(syscheck.wdata.ignored_paths, path, &fields_number, "ignored");
                            break;
                        } else {
                            // The file is new and has to be notified
                        }
                    } else {
                        // Is a directory
                        is_directory = 1;
                    }
                } else {
                    if (s_node->dir_position < 0) {
                        merror(FIM_ERROR_WHODATA_NOTFIND_DIRPOS, path);
                        goto clean;
                    }

                    // Check if the file belongs to a directory that has been transformed to real-time
                    if (!(syscheck.wdata.dirs_status[s_node->dir_position].status & WD_CHECK_WHODATA)) {
                        mdebug2(FIM_WHODATA_CANCELED, path);
                        whodata_hash_add(syscheck.wdata.ignored_paths, path, &fields_number, "ignored");
                        goto clean;
                    }

                    // If the file or directory is already in the hash table, it is not necessary to set its position
                    if (check_path_type(path) == 2) {
                        is_directory = 1;
                    }
                }

                os_calloc(1, sizeof(whodata_evt), w_evt);
                w_evt->user_name = user_name;
                w_evt->user_id = user_id;
                if (!is_directory) {
                    w_evt->path = path;
                    path = NULL;
                } else {
                    // The directory path will be saved in 4663 event
                    w_evt->path = NULL;
                }
                w_evt->dir_position = position;
                w_evt->process_name = process_name;
                w_evt->process_id = process_id;
                w_evt->mask = 0;
                w_evt->scan_directory = is_directory;
                w_evt->deleted = 0;
                w_evt->ignore_not_exist = 0;
                w_evt->ppid = -1;
                w_evt->wnode = whodata_list_add(strdup(hash_id));

                user_name = NULL;
                user_id = NULL;
                process_name = NULL;
add_whodata_evt:
                if (result = whodata_hash_add(syscheck.wdata.fd, hash_id, w_evt, "whodata"), result != 2) {
                    if (result == 1) {
                        mdebug1(FIM_WHODATA_HANDLE_UPDATE, hash_id);
                        whodata_evt *w_evtdup;
                        if (w_evtdup = OSHash_Delete_ex(syscheck.wdata.fd, hash_id), w_evtdup) {
                            free_win_whodata_evt(w_evtdup);
                            goto add_whodata_evt;
                        } else {
                            merror(FIM_ERROR_WHODATA_HANDLER_REMOVE, hash_id);
                        }
                    }
                    free_win_whodata_evt(w_evt);
                    retval = 1;
                    goto clean;
                }
            break;
            // Write fd
            case 4663:
                // Check if the mask is relevant
                if (mask) {
                    if (w_evt = OSHash_Get(syscheck.wdata.fd, hash_id), w_evt) {
                        w_evt->mask |= mask;
                        // Check if it is a rename or copy event
                        if (w_evt->scan_directory && (mask & FILE_WRITE_DATA)) {
                            if (w_dir = OSHash_Get_ex(syscheck.wdata.directories, path), w_dir) {
                                // Get the event time
                                if (buffer[8].Type != EvtVarTypeFileTime) {
                                    merror(FIM_WHODATA_PARAMETER, buffer[8].Type, "event_time");
                                    w_evt->scan_directory = 2;
                                    goto clean;
                                }
                                if (!get_file_time(buffer[8].FileTimeVal, &system_time)) {
                                    merror(FIM_ERROR_WHODATA_HANDLER_EVENT, handle_id);
                                    goto clean;
                                }

                                if (!compare_timestamp(&w_dir->timestamp, &system_time)) {
                                    mdebug2(FIM_WHODATA_DIRECTORY_SCANNED, path);
                                    w_evt->scan_directory = 3;
                                    break;
                                }
                                mdebug2(FIM_WHODATA_NEW_FILES, path);
                            } else {
                                // Check if is a valid directory
                                if (position = find_dir_pos(path, 1, CHECK_WHODATA, 1), position < 0) {
                                    mdebug2(FIM_WHODATA_DIRECTORY_DISCARDED, path);
                                    w_evt->scan_directory = 2;
                                    break;
                                }
                                os_calloc(1, sizeof(whodata_directory), w_dir);
                                memset(&w_dir->timestamp, 0, sizeof(SYSTEMTIME));
                                w_dir->position = position;

                                if (result = whodata_hash_add(syscheck.wdata.directories, path, w_dir, "directories"), result != 2) {
                                    w_evt->scan_directory = 2;
                                    free(w_dir);
                                    break;
                                } else {
                                    mdebug2(FIM_WHODATA_CHECK_NEW_FILES, path);
                                }
                            }
                            w_evt->path = path;
                            path = NULL;
                        }
                    } else {
                        // The file was opened before Wazuh started Syscheck.
                    }
                }
            break;
            // Deleted file
            case 4660:
                if (w_evt = OSHash_Get(syscheck.wdata.fd, hash_id), w_evt) {
                    // The file has been deleted
                    w_evt->deleted = 1;
                } else {
                    // The file was opened before Wazuh started Syscheck.
                }
            break;
            // Close fd
            case 4658:
                if (w_evt = OSHash_Delete_ex(syscheck.wdata.fd, hash_id), w_evt) {
                    unsigned int mask = w_evt->mask;
                    if (!w_evt->scan_directory) {
                        if (w_evt->deleted) {
                            // Check if the file has been deleted
                            send_whodata_del(w_evt);
                        } else if (mask & DELETE) {
                            // The file has been moved or renamed
                            send_whodata_del(w_evt);
                        } else if (mask & modify_criteria) {
                            // Check if the file has been modified
                            realtime_checksumfile(w_evt->path, w_evt);
                        } else {
                            // At this point the file can be new or cleaned
                            w_evt->ignore_not_exist = 1;
                            realtime_checksumfile(w_evt->path, w_evt);
                        }
                    } else if (w_evt->scan_directory == 1) { // Directory scan has been aborted if scan_directory is 2
                        // Check that a new file has been added
                        if ((mask & FILE_WRITE_DATA) && w_evt->path && (w_dir = OSHash_Get(syscheck.wdata.directories, w_evt->path))) {
                            GetSystemTime(&w_dir->timestamp);
                            int pos;
                            if (pos = find_dir_pos(w_evt->path, 1, CHECK_WHODATA, 1), pos >= 0) {
                                int diff = fim_find_child_depth(syscheck.dir[pos], w_evt->path);
                                int depth = syscheck.recursion_level[pos] - diff;
                                read_dir(w_evt->path, pos, NULL, depth, 0);
                            }

                            mdebug1(FIM_WHODATA_SCAN, w_evt->path);
                        } else {
                            mdebug2(FIM_WHODATA_NO_NEW_FILES, w_evt->path, w_evt->mask);
                        }
                    } else if (w_evt->scan_directory == 2) {
                        mdebug1(FIM_WHODATA_SCAN_ABORTED, w_evt->path);
                    }
                    free_win_whodata_evt(w_evt);
                } else {
                    // The file was opened before Wazuh started Syscheck.
                }
            break;
            default:
                merror(FIM_ERROR_WHODATA_EVENTID);
                retval = 1;
                goto clean;
        }
    }
    retval = 0;
clean:
    free(user_name);
    free(path);
    free(process_name);
    if (user_id) {
        LocalFree(user_id);
    }
    if (buffer) {
        free(buffer);
    }
    return retval;
}

int whodata_audit_start() {
    // Set the hash table of ignored paths
    if (syscheck.wdata.ignored_paths = OSHash_Create(), !syscheck.wdata.ignored_paths) {
        return 1;
    }
    // Set the hash table of directories
    if (syscheck.wdata.directories = OSHash_Create(), !syscheck.wdata.directories) {
        return 1;
    }
    // Set the hash table of file descriptors
    // We assume that its default value is 1024
    if (syscheck.wdata.fd = OSHash_Create(), !syscheck.wdata.fd) {
        return 1;
    }

    OSHash_SetFreeDataPointer(syscheck.wdata.fd, (void (*)(void *))free_win_whodata_evt);

    memset(&syscheck.wlist, 0, sizeof(whodata_event_list));
    whodata_list_set_values();

    return 0;
}

long unsigned int WINAPI state_checker(__attribute__((unused)) void *_void) {
    int i;
    int exists;
    whodata_dir_status *d_status;
    SYSTEMTIME utc;
    int interval;

    if (!syscheck.wdata.interval_scan) {
        interval = WDATA_DEFAULT_INTERVAL_SCAN;
    } else {
        interval = syscheck.wdata.interval_scan;
    }

    mdebug1(FIM_WHODATA_CHECKTHREAD, interval);

    while (1) {
        for (i = 0; syscheck.dir[i]; i++) {
            exists = 0;
            d_status = &syscheck.wdata.dirs_status[i];

            if (!(syscheck.wdata.dirs_status[i].status & WD_CHECK_WHODATA)) {
                // It is not whodata
                continue;
            }

            switch (check_path_type(syscheck.dir[i])) {
                case 0:
                    // Unknown device type or does not exist
                    exists = 0;
                break;
                case 1:
                    exists = 1;
                    syscheck.wdata.dirs_status[i].object_type = WD_STATUS_FILE_TYPE;
                break;
                case 2:
                    exists = 1;
                    syscheck.wdata.dirs_status[i].object_type = WD_STATUS_DIR_TYPE;
                break;

            }

            if (exists) {
                if (!(d_status->status & WD_STATUS_EXISTS)) {
                    minfo(FIM_WHODATA_READDED, syscheck.dir[i]);
                    if (set_winsacl(syscheck.dir[i], i)) {
                        merror(FIM_ERROR_WHODATA_ADD_DIRECTORY, syscheck.dir[i]);
                        continue;
                    }
                    d_status->status |= WD_STATUS_EXISTS;
                } else {
                    if (get_creation_date(syscheck.dir[i], &utc)) {
                        merror(FIM_ERROR_WHODATA_CREATION_DATE, syscheck.dir[i]);
                        continue;
                    }

                    if (compare_timestamp(&d_status->last_check, &utc)) {
                        mdebug1(FIM_WHODATA_DEL_ADD, syscheck.dir[i]);
                        if (set_winsacl(syscheck.dir[i], i)) {
                            merror(FIM_ERROR_WHODATA_ADD_DIRECTORY, syscheck.dir[i]);
                            continue;
                        }
                    } else {
                        if (check_object_sacl(syscheck.dir[i], (d_status->object_type == WD_STATUS_FILE_TYPE) ? 1 : 0)) {
                            minfo(FIM_WHODATA_SACL_CHANGED, syscheck.dir[i]);
                            // Mark the directory to prevent its children from sending partial whodata alerts
                            syscheck.wdata.dirs_status[i].status |= WD_CHECK_REALTIME;
                            syscheck.wdata.dirs_status[i].status &= ~WD_CHECK_WHODATA;
                            // Removes CHECK_WHODATA from directory properties to prevent from being found in the whodata callback for Windows (find_dir_pos)
                            syscheck.opts[i] = syscheck.opts[i] & ~CHECK_WHODATA;
                            // Mark it to prevent the restoration of its SACL
                            syscheck.wdata.dirs_status[i].status &= ~WD_IGNORE_REST;
                            notify_SACL_change(syscheck.dir[i]);
                            continue;
                        } else {
                            // The SACL is valid
                        }
                    }
                }
            } else {
                minfo(FIM_WHODATA_DELETE, syscheck.dir[i]);
                d_status->status &= ~WD_STATUS_EXISTS;
                d_status->object_type = WD_STATUS_UNK_TYPE;
            }
            // Set the timestamp
            GetSystemTime(&d_status->last_check);
        }
        sleep(interval);
    }

    return 0;
}

whodata_event_node *whodata_list_add(char *id) {
    whodata_event_node *node = NULL;
    if (syscheck.wlist.current_size < syscheck.wlist.max_size) {
        if (!syscheck.wlist.alerted && syscheck.wlist.alert_threshold < syscheck.wlist.current_size) {
            syscheck.wlist.alerted = 1;
            mwarn("Real-time Whodata events queue for Windows has more than %d elements.", syscheck.wlist.alert_threshold);
        }
    } else {
        mdebug1(FIM_WHODATA_FULLQUEUE, syscheck.wlist.max_remove);
        whodata_list_remove_multiple(syscheck.wlist.max_remove);
    }
    os_calloc(sizeof(whodata_event_node), 1, node);
    if (syscheck.wlist.last) {
        node->next = NULL;
        node->previous = syscheck.wlist.last;
        syscheck.wlist.last = node;
    } else {
        node->next = node->previous = NULL;
        syscheck.wlist.last = syscheck.wlist.first = node;
    }
    node->handle_id = id;
    syscheck.wlist.current_size++;

    return node;
}

void whodata_list_remove_multiple(size_t quantity) {
    size_t i;
    whodata_evt *w_evt;
    for (i = 0; i < quantity && syscheck.wlist.first; i++) {
        if (w_evt = OSHash_Delete_ex(syscheck.wdata.fd, syscheck.wlist.first->handle_id), w_evt) {
            free_whodata_event(w_evt);
        }
        whodata_list_remove(syscheck.wlist.first);
    }
    mdebug1(FIM_WHODATA_EVENT_DELETED, quantity);
}

void whodata_list_remove(whodata_event_node *node) {
    if (!(node->next || node->previous)) {
        syscheck.wlist.first = syscheck.wlist.last = NULL;
    } else {
        if (node->next) {
            if (node->previous) {
                node->next->previous = node->previous;
            } else {
                node->next->previous = NULL;
                syscheck.wlist.first = node->next;
            }
        }

        if (node->previous) {
            if (node->next) {
                node->previous->next = node->next;
            } else {
                node->previous->next = NULL;
                syscheck.wlist.last = node->previous;
            }
        }
    }

    free(node->handle_id);
    free(node);
    syscheck.wlist.current_size--;

    if (syscheck.wlist.alerted && syscheck.wlist.alert_threshold > syscheck.wlist.current_size) {
        syscheck.wlist.alerted = 0;
    }
}

void whodata_list_set_values() {
    syscheck.wlist.max_size = WLIST_MAX_SIZE;
    syscheck.wlist.max_remove = syscheck.wlist.max_size * WLIST_REMOVE_MAX * 0.01;
    syscheck.wlist.alert_threshold = syscheck.wlist.max_size * WLIST_ALERT_THRESHOLD * 0.01;
    mdebug1(FIM_WHODATA_EVENTQUEUE_VALUES
    syscheck.wlist.max_size, syscheck.wlist.max_remove, syscheck.wlist.alert_threshold);
}

void send_whodata_del(whodata_evt *w_evt) {
    static char del_msg[PATH_MAX + OS_SIZE_6144 + 6];
    static char wd_sum[OS_SIZE_6144 + 1];
    syscheck_node *s_node;
    int pos;

    // Remove the file from the syscheck hash table
    if (s_node = OSHash_Delete_ex(syscheck.fp, w_evt->path), !s_node) {
        return;
    }

    free(s_node->checksum);
    free(s_node);

    if (extract_whodata_sum(w_evt, wd_sum, OS_SIZE_6144)) {
        merror(FIM_ERROR_WHODATA_SUM_MAX, w_evt->path);
    }

    /* Find tag if defined for this file */
    pos = find_dir_pos(w_evt->path, 1, 0, 0);

    snprintf(del_msg, PATH_MAX + OS_SIZE_6144 + 6, "-1!%s:%s %s", wd_sum, syscheck.tag[pos] ? syscheck.tag[pos] : "", w_evt->path);
    send_syscheck_msg(del_msg);
}

int set_policies() {
    char *output = NULL;
    int result_code = 0;
    FILE *f_backup = NULL;
    FILE *f_new = NULL;
    char buffer[OS_MAXSTR];
    char command[OS_SIZE_1024];
    int retval = 1;
    static const char *WPOL_FILE_SYSTEM_SUC = ",System,File System,{0CCE921D-69AE-11D9-BED3-505054503030},,,1\n";
    static const char *WPOL_HANDLE_SUC = ",System,Handle Manipulation,{0CCE9223-69AE-11D9-BED3-505054503030},,,1\n";

    if (!IsFile(WPOL_BACKUP_FILE) && remove(WPOL_BACKUP_FILE)) {
        merror(FIM_ERROR_WPOL_BACKUP_FILE_REMOVE, WPOL_BACKUP_FILE, strerror(errno), errno);
        goto end;
    }

    snprintf(command, OS_SIZE_1024, WPOL_BACKUP_COMMAND, WPOL_BACKUP_FILE);

    // Get the current policies
    if (wm_exec(command, &output, &result_code, 5, NULL), result_code) {
        retval = 2;
        goto end;
    }

    free(output);
    output = NULL;

    if (f_backup = fopen (WPOL_BACKUP_FILE, "r"), !f_backup) {
        merror(FIM_ERROR_WPOL_BACKUP_FILE_OPEN, WPOL_BACKUP_FILE, strerror(errno), errno);
        goto end;
    }
    if (f_new = fopen (WPOL_NEW_FILE, "w"), !f_new) {
        merror(FIM_ERROR_WPOL_BACKUP_FILE_REMOVE, WPOL_NEW_FILE, strerror(errno), errno);
        goto end;
    }

    // Copy the policies
    while (fgets(buffer, OS_MAXSTR - 60, f_backup)) {
        fprintf(f_new, buffer);
    }

    // Add the new policies
    fprintf(f_new, WPOL_FILE_SYSTEM_SUC);
    fprintf(f_new, WPOL_HANDLE_SUC);

    fclose(f_new);

    snprintf(command, OS_SIZE_1024, WPOL_RESTORE_COMMAND, WPOL_NEW_FILE);

    // Set the new policies
    if (wm_exec(command, &output, &result_code, 5, NULL), result_code) {
        retval = 2;
        goto end;
    }

    retval = 0;
    restore_policies = 1;
end:
    free(output);
    if (f_backup) {
        fclose(f_backup);
    }
    return retval;
}

void set_subscription_query(wchar_t *query) {
    snwprintf(query, OS_MAXSTR, L"Event[ System[band(Keywords, %llu)] " \
                                    "and " \
                                        "( " \
                                            "( " \
                                                "( " \
                                                    "EventData/Data[@Name='ObjectType'] = 'File' " \
                                                ") " \
                                            "and " \
                                                "( " \
                                                    "(  " \
                                                        "System/EventID = 4656 " \
                                                    "or " \
                                                        "System/EventID = 4663 " \
                                                    ") " \
                                                "and " \
                                                    "( " \
                                                        "EventData[band(Data[@Name='AccessMask'], %lu)] " \
                                                    ") " \
                                                ") " \
                                            ") " \
                                        "or " \
                                            "System/EventID = 4658 " \
                                        "or " \
                                            "System/EventID = 4660 " \
                                        ") " \
                                    "]",
            AUDIT_SUCCESS, // Only successful events
            criteria); // For 4663 and 4656 events need write, delete, change_attributes or change_permissions accesss
}

int get_file_time(unsigned long long file_time_val, SYSTEMTIME *system_time) {
    FILETIME file_time;
    file_time.dwHighDateTime = (DWORD)((file_time_val >> 32) & 0xFFFFFFFF);
    file_time.dwLowDateTime = (DWORD)(file_time_val & 0xFFFFFFFF);
    return FileTimeToSystemTime(&file_time, system_time);
}

int compare_timestamp(SYSTEMTIME *t1, SYSTEMTIME *t2) {
    if (t1->wYear > t2->wYear) {
        return 0;
    } else if (t1->wYear < t2->wYear) {
        return 1;
    }

    if (t1->wMonth > t2->wMonth) {
        return 0;
    } else if (t1->wMonth < t2->wMonth) {
        return 1;
    }

    if (t1->wDay > t2->wDay) {
        return 0;
    } else if (t1->wDay < t2->wDay) {
        return 1;
    }

    if (t1->wHour > t2->wHour) {
        return 0;
    } else if (t1->wHour < t2->wHour) {
        return 1;
    }

    if (t1->wMinute > t2->wMinute) {
        return 0;
    } else if (t1->wMinute < t2->wMinute) {
        return 1;
    }

    if (t1->wSecond > t2->wSecond) {
        return 0;
    } else if (t1->wSecond < t2->wSecond) {
        return 1;
    }

    return 1;
}

void free_win_whodata_evt(whodata_evt *evt) {
    whodata_list_remove(evt->wnode);
    free_whodata_event(evt);
}

int check_object_sacl(char *obj, int is_file) {
    HANDLE hdle = NULL;
    PACL sacl = NULL;
    int retval = 1;
    PSECURITY_DESCRIPTOR security_descriptor = NULL;
    long int result;
    int privilege_enabled = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hdle)) {
        merror(FIM_ERROR_SACL_OPENPROCESSTOKEN, GetLastError());
        return 1;
    }

    if (set_privilege(hdle, priv, TRUE)) {
        merror(FIM_ERROR_SACL_ELEVATE_PRIVILEGE, GetLastError());
        goto end;
    }

    privilege_enabled = 1;
    if (result = GetNamedSecurityInfo(obj, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, &sacl, &security_descriptor), result != ERROR_SUCCESS) {
        merror(FIM_ERROR_SACL_GETSECURITYINFO, result);
        goto end;
    }

    if (is_valid_sacl(sacl, is_file) == 1) {
        // Is a valid SACL
        retval = 0;
    }

end:
    if (privilege_enabled) {
        // Disable the privilege
        if (set_privilege(hdle, priv, FALSE)) {
            merror(FIM_ERROR_SACL_SET_PRIVILEGE, GetLastError());
        }
    }
    if (hdle) {
        CloseHandle(hdle);
    }
    if (security_descriptor) {
        LocalFree((HLOCAL)security_descriptor);
    }
    if (sacl) {
        LocalFree((HLOCAL)sacl);
    }
    return retval;
}

int whodata_hash_add(OSHash *table, char *id, void *data, char *tag) {
    int result;

    if (result = OSHash_Add_ex(table, id, data), result != 2) {
        if (!result) {
            merror(FIM_ERROR_WHODATA_EVENTADD, tag, id);
        } else if (result == 1) {
            merror(FIM_ERROR_WHODATA_EVENTADD_DUP, tag, id);
        }
    }

    return result;
}

void notify_SACL_change(char *dir) {
    char msg_alert[OS_SIZE_1024 + 1];
    snprintf(msg_alert, OS_SIZE_1024, "ossec: Audit: The SACL of '%s' has been modified and can no longer be scanned in whodata mode.", dir);
    SendMSG(syscheck.queue, msg_alert, "syscheck", LOCALFILE_MQ);
}
#endif
