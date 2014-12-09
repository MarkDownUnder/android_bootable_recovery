/*
	Copyright 2013 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <string>
#include <sstream>
#include "../partitions.hpp"
#include "../twrp-functions.hpp"
#include "../openrecoveryscript.hpp"

#include "../adb_install.h"
#ifndef TW_NO_SCREEN_TIMEOUT
#include "blanktimer.hpp"
#endif
extern "C" {
#include "../twcommon.h"
#include "../minuitwrp/minui.h"
#include "../variables.h"
#include "../twinstall.h"
#include "cutils/properties.h"
#include "../minadbd/adb.h"

int TWinstall_zip(const char* path, int* wipe_cache);
void run_script(const char *str1, const char *str2, const char *str3, const char *str4, const char *str5, const char *str6, const char *str7, int request_confirm);
int gui_console_only();
int gui_start();
};

#include "rapidxml.hpp"
#include "objects.hpp"

#ifdef TW_DEVICE_IS_HTC_LEO
#include "../nativeSDmanager.hpp"
TWNativeSDManager NativeSDManager;
#endif

#ifndef TW_NO_SCREEN_TIMEOUT
extern blanktimer blankTimer;
#endif
void curtainClose(void);

GUIAction::GUIAction(xml_node<>* node)
	: Conditional(node)
{
	xml_node<>* child;
	xml_node<>* actions;
	xml_attribute<>* attr;

	mKey = 0;

	if (!node) return;

	// First, get the action
	actions = node->first_node("actions");
	if (actions)
		child = actions->first_node("action");
	else
		child = node->first_node("action");

	if (!child) return;

	while (child) {
		Action action;
		attr = child->first_attribute("function");
		if (!attr) return;	
		action.mFunction = attr->value();
		action.mArg = child->value();
		mActions.push_back(action);
		child = child->next_sibling("action");
	}

	// Now, let's get either the key or region
	child = node->first_node("touch");
	if (child) {
		attr = child->first_attribute("key");
		if (attr) {
			std::string key = attr->value();	
			mKey = getKeyByName(key);
		} else {
			attr = child->first_attribute("x");
			if (!attr)  return;
			mActionX = atol(attr->value());
			attr = child->first_attribute("y");
			if (!attr)  return;
			mActionY = atol(attr->value());
			attr = child->first_attribute("w");
			if (!attr)  return;
			mActionW = atol(attr->value());
			attr = child->first_attribute("h");
			if (!attr)  return;
			mActionH = atol(attr->value());
		}
	}
}

int GUIAction::NotifyTouch(TOUCH_STATE state, int x, int y) {
	if (state == TOUCH_RELEASE)
		doActions();

	return 0;
}

int GUIAction::NotifyKey(int key) {
	if (!mKey || key != mKey)
		return 1;

	doActions();
	return 0;
}

int GUIAction::NotifyVarChange(std::string varName, std::string value) {
	if (varName.empty() && !isConditionValid() && !mKey && !mActionW)
		doActions();

	// This handles notifying the condition system of page start
	if (varName.empty() && isConditionValid())
		NotifyPageSet();

	if ((varName.empty() || IsConditionVariable(varName)) && isConditionValid() && isConditionTrue())
		doActions();

	return 0;
}

void GUIAction::simulate_progress_bar(void) {
	gui_print("Simulating actions...\n");
	for (int i = 0; i < 5; i++) {
		usleep(500000);
		DataManager::SetValue("ui_progress", i * 20);
	}
}

int GUIAction::flash_zip(std::string filename, std::string pageName, const int simulate, int* wipe_cache) {
	int ret_val = 0;

	DataManager::SetValue("ui_progress", 0);

	if (filename.empty()) {
		LOGERR("No file specified.\n");
		return -1;
	}

	// We're going to jump to this page first, like a loading page
	gui_changePage(pageName);

	int fd = -1;
	ZipArchive zip;

	if (!PartitionManager.Mount_By_Path(filename, true))
		return -1;

	if (mzOpenZipArchive(filename.c_str(), &zip)) {
		LOGERR("Unable to open zip file.\n");
		return -1;
	}

	// Check the zip to see if it has a custom installer theme
	const ZipEntry* twrp = mzFindZipEntry(&zip, "META-INF/teamwin/twrp.zip");
	if (twrp != NULL) {
		unlink("/tmp/twrp.zip");
		fd = creat("/tmp/twrp.zip", 0666);
	}
	if (fd >= 0 && twrp != NULL && 
		mzExtractZipEntryToFile(&zip, twrp, fd) && 
		!PageManager::LoadPackage("install", "/tmp/twrp.zip", "main")) {
		mzCloseZipArchive(&zip);
		PageManager::SelectPackage("install");
		gui_changePage("main");
	} else {
		// In this case, we just use the default page
		mzCloseZipArchive(&zip);
		gui_changePage(pageName);
	}
	if (fd >= 0)
		close(fd);

	if (simulate) {
		simulate_progress_bar();
	} else {
		ret_val = TWinstall_zip(filename.c_str(), wipe_cache);

		// Now, check if we need to ensure TWRP remains installed...
		struct stat st;
		if (stat("/sbin/installTwrp", &st) == 0) {
			DataManager::SetValue("tw_operation", "Configuring TWRP");
			DataManager::SetValue("tw_partition", "");
			gui_print("Configuring TWRP...\n");
			if (TWFunc::Exec_Cmd("/sbin/installTwrp reinstall") < 0) {
				gui_print("Unable to configure TWRP with this kernel.\n");
			}
		}
	}

	// Done
	DataManager::SetValue("ui_progress", 100);
	DataManager::SetValue("ui_progress", 0);
	return ret_val;
}

int GUIAction::doActions() {
	if (mActions.size() < 1)
		return -1;
	if (mActions.size() == 1)
		return doAction(mActions.at(0), 0);

	// For multi-action, we always use a thread
	pthread_t t;
	pthread_attr_t tattr;
	if (pthread_attr_init(&tattr)) {
		LOGERR("Unable to pthread_attr_init\n");
		return -1;
	}
	if (pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE)) {
		LOGERR("Error setting pthread_attr_setdetachstate\n");
		return -1;
	}
	if (pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM)) {
		LOGERR("Error setting pthread_attr_setscope\n");
		return -1;
	}
	/*if (pthread_attr_setstacksize(&tattr, 524288)) {
		LOGERR("Error setting pthread_attr_setstacksize\n");
		return -1;
	}*/
	int ret = pthread_create(&t, &tattr, thread_start, this);
	if (ret) {
		LOGINFO("Unable to create more threads for actions... continuing in same thread! %i\n", ret);
		thread_start(this);
	} else {
		if (pthread_join(t, NULL))
			LOGERR("Error joining threads\n");
	}
	if (pthread_attr_destroy(&tattr)) {
		LOGERR("Failed to pthread_attr_destroy\n");
		return -1;
	}

	return 0;
}

void* GUIAction::thread_start(void *cookie)
{
	GUIAction* ourThis = (GUIAction*) cookie;

	DataManager::SetValue(TW_ACTION_BUSY, 1);

	if (ourThis->mActions.size() > 1) {
		std::vector<Action>::iterator iter;
		for (iter = ourThis->mActions.begin(); iter != ourThis->mActions.end(); iter++)
			ourThis->doAction(*iter, 1);
	} else {
		ourThis->doAction(ourThis->mActions.at(0), 1);
	}
	int check = 0;
	DataManager::GetValue("tw_background_thread_running", check);
	if (check == 0)
		DataManager::SetValue(TW_ACTION_BUSY, 0);
	return NULL;
}

void GUIAction::operation_start(const string operation_name) {
	DataManager::SetValue(TW_ACTION_BUSY, 1);
	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("tw_operation", operation_name);
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 0);
}

void GUIAction::operation_end(const int operation_status, const int simulate) {
	int simulate_fail;
	DataManager::SetValue("ui_progress", 100);
	if (simulate) {
		DataManager::GetValue(TW_SIMULATE_FAIL, simulate_fail);
		if (simulate_fail != 0)
			DataManager::SetValue("tw_operation_status", 1);
		else
			DataManager::SetValue("tw_operation_status", 0);
	} else {
		if (operation_status != 0) {
			DataManager::SetValue("tw_operation_status", 1);
		}
		else {
			DataManager::SetValue("tw_operation_status", 0);
		}
	}
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
#ifndef TW_NO_SCREEN_TIMEOUT
	if (!DataManager::Pause_For_Battery_Charge())
		blankTimer.resetTimerAndUnblank();
#endif
}

// lame...
std::string GUIAction::Function_Name() {
	std::string func;
	if (mActions.size() < 1)
		func = "";
	else if (mActions.size() >= 1)
		func = mActions.at(0).mFunction;
	return func;
}

int GUIAction::doAction(Action action, int isThreaded /* = 0 */) {
	static string zip_queue[10];
	static int zip_queue_index;
	static pthread_t terminal_command;
	int simulate;

	std::string arg = gui_parse_text(action.mArg);

	std::string function = gui_parse_text(action.mFunction);

	DataManager::GetValue(TW_SIMULATE_ACTIONS, simulate);

	if (function == "reboot") {
		//curtainClose(); this sometimes causes a crash
		sync();
		DataManager::SetValue("tw_gui_done", 1);
		DataManager::SetValue("tw_reboot_arg", arg);
		return 0;
	}
	if (function == "home") {
		PageManager::SelectPackage("TWRP");
		gui_changePage("main");
		return 0;
	}
	if (function == "key") {
		PageManager::NotifyKey(getKeyByName(arg));
		return 0;
	}
	if (function == "page") {
		std::string page_name = gui_parse_text(arg);
		return gui_changePage(page_name);
	}
	if(function == "rotation") {
		int rot = atoi(arg.c_str());
		if (rot == gr_get_rotation())
			return 0;

		operation_start("Rotation");
		int res = gui_rotate(rot);
		operation_end(res != 0, simulate);
		return 0;
	}
	if (function == "reload") {
		operation_start("Reload Theme");
		gui_setRenderEnabled(0);
#ifndef TW_NO_SCREEN_TIMEOUT
		blankTimer.setBlank(0);
#endif
		usleep(1100);
		if (TWFunc::reloadTheme()) {
			gui_setRenderEnabled(1);
#ifndef TW_NO_SCREEN_TIMEOUT
			int timeout = DataManager::GetIntValue("tw_screen_timeout_secs");
			blankTimer.setBlank(1);
			blankTimer.setTimerThread();
			blankTimer.setTime(timeout);
			operation_end(0, simulate);
#endif
		} else
			operation_end(1, simulate);
	}
	if (function == "readBackup") {
		string Restore_Name;
		DataManager::GetValue("tw_restore", Restore_Name);
		PartitionManager.Set_Restore_Files(Restore_Name);
		return 0;
	}
	if (function == "set") {
		if (arg.find('=') != string::npos) {
			string varName = arg.substr(0, arg.find('='));
			string value = arg.substr(arg.find('=') + 1, string::npos);

			DataManager::GetValue(value, value);
			DataManager::SetValue(varName, value);
		} else
			DataManager::SetValue(arg, "1");
		return 0;
	}
	if (function == "save") {
		if (arg.find('=') != string::npos) {
			string varName = arg.substr(0, arg.find('='));
			string value = arg.substr(arg.find('=') + 1, string::npos);

			DataManager::GetValue(value, value);
			DataManager::SetValue(varName, value, 1);
		} else
			DataManager::SetValue(arg, "1", 1);
		return 0;
	}
	if (function == "clear") {
		DataManager::SetValue(arg, "0");
		return 0;
	}
	if (function == "mount") {
		if (arg == "usb") {
			DataManager::SetValue(TW_ACTION_BUSY, 1);
			if (!simulate)
				PartitionManager.usb_storage_enable();
			else
				gui_print("Simulating actions...\n");
		}
		else if (!simulate) {
			string cmd;
			if (arg == "EXTERNAL")
				PartitionManager.Mount_By_Path(DataManager::GetStrValue(TW_EXTERNAL_MOUNT), true);
			else if (arg == "INTERNAL")
				PartitionManager.Mount_By_Path(DataManager::GetStrValue(TW_INTERNAL_MOUNT), true);
			else
				PartitionManager.Mount_By_Path(arg, true);
		} else
			gui_print("Simulating actions...\n");
		return 0;
	}
	if (function == "umount" || function == "unmount") {
		if (arg == "usb") {
			if (!simulate)
				PartitionManager.usb_storage_disable();
			else
				gui_print("Simulating actions...\n");
			DataManager::SetValue(TW_ACTION_BUSY, 0);
		}
		else if (!simulate) {
			string cmd;
			if (arg == "EXTERNAL")
				PartitionManager.UnMount_By_Path(DataManager::GetStrValue(TW_EXTERNAL_MOUNT), true);
			else if (arg == "INTERNAL")
				PartitionManager.UnMount_By_Path(DataManager::GetStrValue(TW_INTERNAL_MOUNT), true);
			else
				PartitionManager.UnMount_By_Path(arg, true);
		} else
			gui_print("Simulating actions...\n");
		return 0;
	}	
	if (function == "copylog") {
		operation_start("Copy Log");
		if (!simulate) {
			string dst;
			PartitionManager.Mount_Current_Storage(true);
			dst = DataManager::GetCurrentStoragePath() + "/recovery.log";
			TWFunc::copy_file("/tmp/recovery.log", dst.c_str(), 0755);
			sync();
			gui_print("Copied recovery log to %s.\n", DataManager::GetCurrentStoragePath().c_str());
		} else
			simulate_progress_bar();
		operation_end(0, simulate);
		return 0;
	}	
	if (function == "compute" || function == "addsubtract") {
		if (arg.find("+") != string::npos) {
			string varName = arg.substr(0, arg.find('+'));
			string string_to_add = arg.substr(arg.find('+') + 1, string::npos);
			int amount_to_add = atoi(string_to_add.c_str());
			int value;

			DataManager::GetValue(varName, value);
			DataManager::SetValue(varName, value + amount_to_add);
			return 0;
		}
		if (arg.find("-") != string::npos) {
			string varName = arg.substr(0, arg.find('-'));
			string string_to_subtract = arg.substr(arg.find('-') + 1, string::npos);
			int amount_to_subtract = atoi(string_to_subtract.c_str());
			int value;

			DataManager::GetValue(varName, value);
			value -= amount_to_subtract;
			if (value <= 0)
				value = 0;
			if (varName == "tw_sdext_size" && value == 0)
				DataManager::SetValue("tw_sdext2_size", 0);
			DataManager::SetValue(varName, value);
			return 0;
		}
		if (arg.find("*") != string::npos)
		{
			string varName = arg.substr(0, arg.find('*'));
			string multiply_by_str = gui_parse_text(arg.substr(arg.find('*') + 1, string::npos));
			int multiply_by = atoi(multiply_by_str.c_str());
			int value;

			DataManager::GetValue(varName, value);
			DataManager::SetValue(varName, value*multiply_by);
			return 0;
		}
		if (arg.find("/") != string::npos)
		{
			string varName = arg.substr(0, arg.find('/'));
			string divide_by_str = gui_parse_text(arg.substr(arg.find('/') + 1, string::npos));
			int divide_by = atoi(divide_by_str.c_str());
			int value;

			if(divide_by != 0)
			{
				DataManager::GetValue(varName, value);
				DataManager::SetValue(varName, value/divide_by);
			}
			return 0;
		}
		LOGERR("Unable to perform compute '%s'\n", arg.c_str());
		return -1;
	}	
	if (function == "setguitimezone") {
		return DataManager::SetGUI_TimeZone();
	}
	if (function == "togglestorage") {
		if (arg == "internal") {
			DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 0);
		} else if (arg == "external") {
			DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 1);
		}
		if (PartitionManager.Mount_Current_Storage(true)) {
			if (arg == "internal") {
				string zip_path, zip_root;
				DataManager::GetValue(TW_ZIP_INTERNAL_VAR, zip_path);
				zip_root = TWFunc::Get_Root_Path(zip_path);
#ifdef RECOVERY_SDCARD_ON_DATA
	#ifndef TW_EXTERNAL_STORAGE_PATH
				if (zip_root != "/sdcard")
					DataManager::SetValue(TW_ZIP_INTERNAL_VAR, "/sdcard");
	#else
				if (strcmp(EXPAND(TW_EXTERNAL_STORAGE_PATH), "/sdcard") == 0) {
					if (zip_root != "/emmc")
						DataManager::SetValue(TW_ZIP_INTERNAL_VAR, "/emmc");
				} else {
					if (zip_root != "/sdcard")
						DataManager::SetValue(TW_ZIP_INTERNAL_VAR, "/sdcard");
				}
	#endif
#else
				if (zip_root != DataManager::GetCurrentStoragePath())
					DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetCurrentStoragePath());
#endif
				// Save the current zip location to the external variable
				DataManager::SetValue(TW_ZIP_EXTERNAL_VAR, DataManager::GetStrValue(TW_ZIP_LOCATION_VAR));
				// Change the current zip location to the internal variable
				DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetStrValue(TW_ZIP_INTERNAL_VAR));
			} else if (arg == "external") {
				string zip_path, zip_root;
				DataManager::GetValue(TW_ZIP_EXTERNAL_VAR, zip_path);
				zip_root = TWFunc::Get_Root_Path(zip_path);
				if (zip_root != DataManager::GetCurrentStoragePath()) {
					DataManager::SetValue(TW_ZIP_EXTERNAL_VAR, DataManager::GetCurrentStoragePath());
				}
				// Save the current zip location to the internal variable
				DataManager::SetValue(TW_ZIP_INTERNAL_VAR, DataManager::GetStrValue(TW_ZIP_LOCATION_VAR));
				// Change the current zip location to the external variable
				DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetStrValue(TW_ZIP_EXTERNAL_VAR));
			}
		} else {
			// We weren't able to toggle for some reason, restore original setting
			if (arg == "internal") {
				DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 1);
			} else if (arg == "external") {
				DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 0);
			}
		}
		return 0;
	}	
	if (function == "overlay")
		return gui_changeOverlay(arg);
	if (function == "queuezip") {
		if (zip_queue_index >= 10) {
			gui_print("Maximum zip queue reached!\n");
			return 0;
		}
		DataManager::GetValue("tw_filename", zip_queue[zip_queue_index]);
		if (strlen(zip_queue[zip_queue_index].c_str()) > 0) {
			zip_queue_index++;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}
	if (function == "cancelzip") {
		if (zip_queue_index <= 0) {
			gui_print("Minimum zip queue reached!\n");
			return 0;
		} else {
			zip_queue_index--;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}
	if (function == "queueclear") {
		zip_queue_index = 0;
		DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		return 0;
	}
	if (function == "sleep") {
		operation_start("Sleep");
		usleep(atoi(arg.c_str()));
		operation_end(0, simulate);
		return 0;
	}
	if (function == "cls") {
		operation_start("Cls");
		gui_cls();
		operation_end(0, simulate);
		return 0;
	}
	if (function == "screenshot") {
		int y, z;
		DataManager::GetValue(TW_ACTION_BUSY, y);
		DataManager::GetValue(TW_SCREENSHOT_VAR, z);
		if (y != 1 && z == 1) {
			string current_storage_path = DataManager::GetCurrentStoragePath();
			if (!PartitionManager.Is_Mounted_By_Path(current_storage_path))
				PartitionManager.Mount_Current_Storage(false);
			if (PartitionManager.Is_Mounted_By_Path(current_storage_path))
				TWFunc::Take_Screenshot();
		}
		return 0;
	}
	if (function == "appenddatetobackupname")
	{
		operation_start("AppendDateToBackupName");
		string Backup_Name;
		DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
		Backup_Name += TWFunc::Get_Current_Date();
		if (Backup_Name.size() > MAX_BACKUP_NAME_LEN)
			Backup_Name.resize(MAX_BACKUP_NAME_LEN);
		DataManager::SetValue(TW_BACKUP_NAME, Backup_Name);
		operation_end(0, simulate);
		return 0;
	}
	if (function == "generatebackupname")
	{
		operation_start("GenerateBackupName");
		TWFunc::Auto_Generate_Backup_Name();
		operation_end(0, simulate);
		return 0;
	}

//Threaded functions
	if (isThreaded)	{
		if (function == "getfilesize") {
			float Size = 0;

			operation_start("FileSize");
			Size = ((float)TWFunc::Get_File_Size(arg) / (float)1048576LLU);
			DataManager::SetValue("tw_filename1_size", Size);
			operation_end(0, simulate);
			return 0;
		}

		if (function == "viewfile") {
			operation_start("ViewFile");
			gui_cls();
			if (gr_get_rotation() % 180 == 0)
				TWFunc::cat_file(arg, 47);
			else
				TWFunc::cat_file(arg, 90);
			operation_end(0, simulate);
			return 0;
		}

		if (function == "getfoldersize") {
			float Size = 0;

			operation_start("FolderSize");
			Size = ((float)TWFunc::Get_Folder_Size(arg, false) / (float)1048576LLU);
			DataManager::SetValue("tw_filename1_size", Size);
			operation_end(0, simulate);
			return 0;
		}

		if (function == "notemptystr") {
			operation_start("StrLenChk");
			if (arg.size() > 0)
				operation_end(0, simulate);				
			else
				operation_end(1, simulate);
			return 0;
		}

		if (function == "emptystr") {
			operation_start("StrLenChk");
			if (arg.size() == 0)
				operation_end(0, simulate);				
			else
				operation_end(1, simulate);
			return 0;
		}

		if (function == "fileexists") {
			struct stat st;
			string newpath = arg + "/.";

			operation_start("FileExists");
			if (stat(arg.c_str(), &st) == 0 || stat(newpath.c_str(), &st) == 0)
				operation_end(0, simulate);
			else
				operation_end(1, simulate);
			return 0;
		}

		if (function == "validbootimg") {
			int ret = 0;
			operation_start("BootImgValidate");
			ret = TWFunc::CheckFor_ValidIMG(arg);
			if (ret > 0)
				ret = 0; // 0 for success
			else
				ret = 1; // 1 for failure
			operation_end(ret, simulate);
			return 0;
		}

		if (function == "app_run") {
			int ret = 0, wipe_cache = 0;;
			string appfile;
			DataManager::GetValue("tw_app_file", appfile);

			operation_start("AppExec");
			ret = flash_zip(appfile, arg, simulate, &wipe_cache);
			if (ret != 0) {
				gui_print("Error with zip '%s'\n", appfile.c_str());				
				ret = 1;
			}
		   	operation_end(ret, simulate);
			return 0;
		}
		if (function == "flash") {
			int i, ret_val = 0, wipe_cache = 0;

			for (i=0; i<zip_queue_index; i++) {
				operation_start("Flashing");
				DataManager::SetValue("tw_filename", zip_queue[i]);
				DataManager::SetValue(TW_ZIP_INDEX, (i + 1));

				ret_val = flash_zip(zip_queue[i], arg, simulate, &wipe_cache);
				if (ret_val != 0) {
					gui_print("Error flashing zip '%s'\n", zip_queue[i].c_str());
					i = 10; // Error flashing zip - exit queue
					ret_val = 1;
				}
			}

			if (wipe_cache)
				PartitionManager.Wipe_By_Path("/cache");
			string result;
			if (DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) {
				operation_start("ReinjectTWRP");
				gui_print("Injecting TWRP into boot image...\n");
				if (simulate) {
					simulate_progress_bar();
				} else {
					string injectcmd = "";
					TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
					if (Boot == NULL || Boot->Current_File_System != "emmc")
						injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash";
					else
						injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash bd=" + Boot->Actual_Block_Device;
					TWFunc::Exec_Cmd(injectcmd);
					gui_print("TWRP injection complete.\n");
				}
			}
			PartitionManager.Update_System_Details(true);
			zip_queue_index = 0;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
			DataManager::SetValue(TW_ZIP_INDEX, 0);
			operation_end(ret_val, simulate);
			TWFunc::Vibrate(install_completed);
			return 0;
		}
		if (function == "wipe")	{
			operation_start("Format");
			DataManager::SetValue("tw_partition", arg);

			int ret_val = false;

			if (simulate) {
				simulate_progress_bar();
			} else {
				if (arg == "data")
					ret_val = PartitionManager.Factory_Reset();
				else if (arg == "all")
					ret_val = PartitionManager.Wipe_All_But_SDCARD();
				else if (arg == "battery")
					ret_val = PartitionManager.Wipe_Battery_Stats();
				else if (arg == "rotate")
					ret_val = PartitionManager.Wipe_Rotate_Data();
				else if (arg == "dalvik")
					ret_val = PartitionManager.Wipe_Dalvik_Cache();
				else if (arg == "DATAMEDIA") {
					ret_val = PartitionManager.Format_Data();
				} else if (arg == "INTERNAL") {
					int has_datamedia, dual_storage;

					DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
					if (has_datamedia) {
						ret_val = PartitionManager.Wipe_Media_From_Data();
					} else {
						ret_val = PartitionManager.Wipe_By_Path(DataManager::GetSettingsStoragePath());
					}
				} else if (arg == "EXTERNAL") {
					string External_Path;

					DataManager::GetValue(TW_EXTERNAL_PATH, External_Path);
					ret_val = PartitionManager.Wipe_By_Path(External_Path);
				} else if (arg == "ANDROIDSECURE") {
					ret_val = PartitionManager.Wipe_Android_Secure();
				} else if (arg == "LIST") {
					string Wipe_List, wipe_path;
					bool skip = false;
					ret_val = true;
					TWPartition* wipe_part = NULL;

					DataManager::GetValue("tw_wipe_list", Wipe_List);
					LOGINFO("wipe list '%s'\n", Wipe_List.c_str());
					if (!Wipe_List.empty()) {
						size_t start_pos = 0, end_pos = Wipe_List.find(";", start_pos);
						while (end_pos != string::npos && start_pos < Wipe_List.size()) {
							wipe_path = Wipe_List.substr(start_pos, end_pos - start_pos);
							LOGINFO("wipe_path '%s'\n", wipe_path.c_str());
							if (wipe_path == "/and-sec") {
								if (!PartitionManager.Wipe_Android_Secure()) {
									LOGERR("Unable to wipe android secure\n");
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							} else if (wipe_path == "DALVIK") {
								if (!PartitionManager.Wipe_Dalvik_Cache()) {
									LOGERR("Failed to wipe dalvik\n");
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							} else if (wipe_path == "INTERNAL") {
								if (!PartitionManager.Wipe_Media_From_Data()) {
									LOGERR("Failed to wipe internal media\n");
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							}
							if (!skip) {
								if (!PartitionManager.Wipe_By_Path(wipe_path)) {
									LOGERR("Unable to wipe '%s'\n", wipe_path.c_str());
									ret_val = false;
									break;
								} else if (wipe_path == DataManager::GetSettingsStoragePath()) {
									arg = wipe_path;
								}
							} else {
								skip = false;
							}
							start_pos = end_pos + 1;
							end_pos = Wipe_List.find(";", start_pos);
						}
					}
				} else
					ret_val = PartitionManager.Wipe_By_Path(arg);

				if (arg == DataManager::GetSettingsStoragePath()) {
					// If we wiped the settings storage path, recreate the TWRP folder and dump the settings
					string Storage_Path = DataManager::GetSettingsStoragePath();

					if (PartitionManager.Mount_By_Path(Storage_Path, true)) {
						LOGINFO("Making TWRP folder and saving settings.\n");
						Storage_Path += "/TWRP";
						mkdir(Storage_Path.c_str(), 0777);
						DataManager::SetupTwrpFolder();
						DataManager::Flush();
					} else {
						LOGERR("Unable to recreate TWRP folder and save settings.\n");
					}
				}
			}
			PartitionManager.Update_System_Details(true);
			if (ret_val) {
				ret_val = 0; // 0 is success
				TWFunc::Vibrate((FeedbackReason)150);
			} else
				ret_val = 1; // 1 is failure
			operation_end(ret_val, simulate);
			return 0;
		}
		if (function == "refreshsizes")	{
			operation_start("Refreshing Sizes");
			if (simulate) {
				simulate_progress_bar();
			} else {
				PartitionManager.Update_System_Details(true);
				if (arg == "forcerender")
					gui_forceRender();
			}
			operation_end(0, simulate);
			return 0;
		}
		if (function == "nandroid") {
			operation_start("Nandroid");
			int ret = 0;

			if (simulate) {
				DataManager::SetValue("tw_partition", "Simulation");
				simulate_progress_bar();
			} else {
				if (arg == "backup") {
					string Backup_Name;
					DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
					if (Backup_Name == "(Auto Generate)" || Backup_Name == "(Current Date)" || Backup_Name == "0" || Backup_Name == "(" || PartitionManager.Check_Backup_Name(true) == 0) {
						ret = PartitionManager.Run_Backup();
					}
					else {
						operation_end(1, simulate);
						return -1;
					}
					DataManager::SetValue(TW_BACKUP_NAME, "(Current Date)");
					TWFunc::Vibrate(backup_completed);
				} else if (arg == "restore") {
					string Restore_Name;
					DataManager::GetValue("tw_restore", Restore_Name);
					ret = PartitionManager.Run_Restore(Restore_Name);
					TWFunc::Vibrate(restore_completed);
				} else {
					operation_end(1, simulate);
					return -1;
				}
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
				TWFunc::drop_caches("3\n");
			}
			operation_end(ret, simulate);
			return 0;
		}
		if (function == "script_run") {
			int ret = 0;
			string scrfile;
			DataManager::GetValue("tw_script_file", scrfile);
			string command = "chmod 0775 " + scrfile + " && " + scrfile;
			LOGINFO("ScriptExec: %s.\n", command.c_str());

			operation_start("ScriptExec");
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = system(command.c_str());
			}
			if (ret != 0)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
			}
		   	operation_end(ret, simulate);

			return 0;
		}
#ifdef TW_DEVICE_IS_HTC_LEO
		if (function == "nativeboot_run") {
			int ret = 0;
			string tmp, sdrompath;

			DataManager::GetValue("sd_rom1", sdrompath);
			operation_start("NativeSDManager");
			gui_print("Preparing kernel before rebooting...\n");
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Prep_Rom_To_Boot(sdrompath);
			}

			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
			}
		   	operation_end(ret, simulate);
			sync();
			DataManager::SetValue("tw_gui_done", 1);
			DataManager::SetValue("tw_reboot_arg", "system");
			return 0;
		}
		if (function == "nativekernel_run") {
			int ret = 0;
			string ptn, sdkernelpath;

			DataManager::GetValue("tw_sdboot_partition", ptn);
			DataManager::GetValue("sd_kernel", sdkernelpath);

			operation_start("NativeSDManager");
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Kernel_Update(ptn, sdkernelpath);
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
			}
		   	operation_end(ret, simulate);

			return 0;
		}
		if (function == "nativebackup_run") {
			int i, ret = 0;
			string tmp, sdrompath;

			DataManager::GetValue("sd_rom", tmp);
			DataManager::GetValue(TW_USE_SDEXT2_PARTITION, i);
			if (i == 0) {
				PartitionManager.Mount_By_Path("/sd-ext", true);
				sdrompath = "/sd-ext" + tmp.substr(16, tmp.size() - 1);
			} else {
				PartitionManager.Mount_By_Path("/sdext2", true);
				sdrompath = "/sdext2" + tmp.substr(16, tmp.size() - 1);
			}
			operation_start("NativeSDManager");
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Backup(sdrompath);
			}

			if (i == 0) {
				PartitionManager.UnMount_By_Path("/sd-ext", true);
			} else {
				PartitionManager.UnMount_By_Path("/sdext2", true);
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
				TWFunc::Vibrate(sdbackup_completed);
				TWFunc::drop_caches("3\n");
			}
		   	operation_end(ret, simulate);

			return 0;
		}
		if (function == "nativerestore_run") {
			int i, ret = 0;
			string sdrompath;

			DataManager::GetValue("sd_rom2", sdrompath);
			DataManager::GetValue(TW_USE_SDEXT2_PARTITION, i);
			if (i == 0) {
				PartitionManager.Mount_By_Path("/sd-ext", true);
			} else {
				PartitionManager.Mount_By_Path("/sdext2", true);
			}
			operation_start("NativeSDManager");
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Restore(sdrompath);
			}

			if (i == 0) {
				PartitionManager.UnMount_By_Path("/sd-ext", true);
			} else {
				PartitionManager.UnMount_By_Path("/sdext2", true);
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
				TWFunc::Vibrate(sdrestore_completed);
				TWFunc::drop_caches("3\n");
			}
		   	operation_end(ret, simulate);

			return 0;
		}
		if (function == "nativedelete_run") {
			int i, ret = 0;
			string tmp, sdrompath;

			DataManager::GetValue("sd_rom3", tmp);
			DataManager::GetValue(TW_USE_SDEXT2_PARTITION, i);
			if (i == 0) {
				PartitionManager.Mount_By_Path("/sd-ext", true);
				sdrompath = "/sd-ext" + tmp.substr(16, tmp.size() - 1);
			} else {
				PartitionManager.Mount_By_Path("/sdext2", true);
				sdrompath = "/sdext2" + tmp.substr(16, tmp.size() - 1);
			}
			operation_start("NativeSDManager");
			gui_print("Deleting %s\n", sdrompath.c_str());
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Delete(sdrompath);
			}

			if (i == 0) {
				PartitionManager.UnMount_By_Path("/sd-ext", true);
			} else {
				PartitionManager.UnMount_By_Path("/sdext2", true);
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
			}
		   	operation_end(ret, simulate);

			return 0;
		}
		if (function == "nativeperm_run") {
			int i, ret = 0;
			string tmp, sdrompath;

			DataManager::GetValue("sd_rom4", tmp);
			DataManager::GetValue(TW_USE_SDEXT2_PARTITION, i);
			if (i == 0) {
				PartitionManager.Mount_By_Path("/sd-ext", true);
				sdrompath = "/sd-ext" + tmp.substr(16, tmp.size() - 1);
			} else {
				PartitionManager.Mount_By_Path("/sdext2", true);
				sdrompath = "/sdext2" + tmp.substr(16, tmp.size() - 1);
			}
			operation_start("NativeSDManager");
			gui_print("Fixing permissions on %s\n", sdrompath.c_str());
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Fix_Perm(sdrompath);
			}

			if (i == 0) {
				PartitionManager.UnMount_By_Path("/sd-ext", true);
			} else {
				PartitionManager.UnMount_By_Path("/sdext2", true);
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
			}
		   	operation_end(ret, simulate);

			return 0;
		}
		if (function == "nativewipedata_run") {
			int i, ret = 0;
			string tmp, sdrompath;

			DataManager::GetValue("sd_rom5", tmp);
			DataManager::GetValue(TW_USE_SDEXT2_PARTITION, i);
			if (i == 0) {
				PartitionManager.Mount_By_Path("/sd-ext", true);
				sdrompath = "/sd-ext" + tmp.substr(16, tmp.size() - 1);
			} else {
				PartitionManager.Mount_By_Path("/sdext2", true);
				sdrompath = "/sdext2" + tmp.substr(16, tmp.size() - 1);
			}
			operation_start("NativeSDManager");
			gui_print("Wiping data on %s\n", sdrompath.c_str());
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Wipe_Data(sdrompath);
			}

			if (i == 0) {
				PartitionManager.UnMount_By_Path("/sd-ext", true);
			} else {
				PartitionManager.UnMount_By_Path("/sdext2", true);
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
			}
		   	operation_end(ret, simulate);

			return 0;
		}
		if (function == "nativewipedalvik_run") {
			int i, ret = 0;
			string tmp, sdrompath;

			DataManager::GetValue("sd_rom6", tmp);
			DataManager::GetValue(TW_USE_SDEXT2_PARTITION, i);
			if (i == 0) {
				PartitionManager.Mount_By_Path("/sd-ext", true);
				sdrompath = "/sd-ext" + tmp.substr(16, tmp.size() - 1);
			} else {
				PartitionManager.Mount_By_Path("/sdext2", true);
				sdrompath = "/sdext2" + tmp.substr(16, tmp.size() - 1);
			}
			operation_start("NativeSDManager");
			gui_print("Wiping dalvik-cache on %s\n", sdrompath.c_str());
			if (simulate) {
				simulate_progress_bar();
				operation_end(ret, simulate);
			} else {
				ret = NativeSDManager.Wipe_Dalvik(sdrompath);
			}

			if (i == 0) {
				PartitionManager.UnMount_By_Path("/sd-ext", true);
			} else {
				PartitionManager.UnMount_By_Path("/sdext2", true);
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else {
				ret = 0; // 0 for success
			}
		   	operation_end(ret, simulate);

			return 0;
		}
#endif
		if (function == "restoredefaultsettings") {
			operation_start("Restore Defaults");
			if (simulate) // Simulated so that people don't accidently wipe out the "simulation is on" setting
				gui_print("Simulating actions...\n");
			else {
				gui_print("Default settings restored.\n");
				DataManager::ResetDefaults();
				// koko: Since we are going to reboot are these needed?
				PartitionManager.Update_System_Details(true);
				PartitionManager.Mount_Current_Storage(true);
			}
			operation_end(0, simulate);
			sync();
			gui_print("Rebooting device...\n");
			sleep(2);
			DataManager::SetValue("tw_gui_done", 1);
			DataManager::SetValue("tw_reboot_arg", "recovery");

			return 0;
		}
		if (function == "fixpermissions") {
			operation_start("Fix Permissions");
			int op_status = 0;
					
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = PartitionManager.Fix_Permissions();
				if (op_status != 0)
					op_status = 1; // failure
			}
			operation_end(op_status, simulate);

			return 0;
		}
		if (function == "dd")
		{
			operation_start("imaging");

			if (simulate) {
				simulate_progress_bar();
			} else {
				string cmd = "dd " + arg;
				TWFunc::Exec_Cmd(cmd);
			}
			operation_end(0, simulate);
			return 0;
		}
		if (function == "partitionsd")
		{
			operation_start("Partition SD Card");
			int ret_val = 0;

			if (simulate) {
				simulate_progress_bar();
			} else {
				int allow_partition;
				DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
				if (allow_partition == 0) {
					gui_print("This device does not have a real SD Card!\nAborting!\n");
				} else {
					if (!PartitionManager.Partition_SDCard())
						ret_val = 1; // failed
				}
			}
			operation_end(ret_val, simulate);
			TWFunc::Vibrate(parted_completed);
			return 0;
		}
		if (function == "convertextfs") {
			operation_start("Converting filesystem of EXT partition");
			int ret_val = 0;

			if (simulate) {
				simulate_progress_bar();
			} else {
				int allow_partition;
				DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
				if (allow_partition == 0) {
					gui_print("This device does not have a real SD Card!\nAborting!\n");
				} else {
					if (!PartitionManager.FSConvert_SDEXT("/sd-ext"))
						ret_val = 1; // failed
				}
			}
			operation_end(ret_val, simulate);
			TWFunc::Vibrate((FeedbackReason)150);
			return 0;
		}
		if (function == "convertext2fs") {
			operation_start("Converting filesystem of 2nd EXT partition");
			int ret_val = 0;

			if (simulate) {
				simulate_progress_bar();
			} else {
				int z;
				DataManager::GetValue(TW_HAS_SDEXT2_PARTITION, z);
				if (z == 1) {
					int allow_partition;
					DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
					if (allow_partition == 0) {
						gui_print("This device does not have a real SD Card!\nAborting!\n");
					} else {
						if (!PartitionManager.FSConvert_SDEXT("/sdext2"))
							ret_val = 1; // failed
					}
				}
			}
			operation_end(ret_val, simulate);
			TWFunc::Vibrate((FeedbackReason)150);
			return 0;
		}	
		if (function == "fschk") {
			operation_start("FS-Check");
			int ret_val = 0;

			if (simulate) {
				simulate_progress_bar();
			} else {
				if (!PartitionManager.Check_SDCard())
					ret_val = 1; // failed
			}
			TWFunc::Vibrate((FeedbackReason)150);
			operation_end(ret_val, simulate);

			return 0;
		}	
		if (function == "installhtcdumlock") {
			operation_start("Install HTC Dumlock");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::install_htc_dumlock();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockrestoreboot")
		{
			operation_start("HTC Dumlock Restore Boot");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::htc_dumlock_restore_original_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockreflashrecovery")
		{
			operation_start("HTC Dumlock Reflash Recovery");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::htc_dumlock_reflash_recovery_to_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "cmd")
		{
			int op_status = 0;
			string result;

			operation_start("Command");
			LOGINFO("Running command: '%s'\n", arg.c_str());
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = TWFunc::Exec_Cmd(arg, result);
				if (op_status != 0)
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "terminalcommand")
		{
			int op_status = 0;
			string cmdpath, command;

			DataManager::GetValue("tw_terminal_location", cmdpath);
			operation_start("CommandOutput");
			gui_print("%s # %s\n", cmdpath.c_str(), arg.c_str());
			if (simulate) {
				simulate_progress_bar();
				operation_end(op_status, simulate);
			} else {
				command = "cd \"" + cmdpath + "\" && " + arg + " 2>&1";;
				LOGINFO("Actual command is: '%s'\n", command.c_str());
				DataManager::SetValue("tw_terminal_command_thread", command);
				DataManager::SetValue("tw_terminal_state", 1);
				DataManager::SetValue("tw_background_thread_running", 1);
				op_status = pthread_create(&terminal_command, NULL, command_thread, NULL);
				if (op_status != 0) {
					LOGERR("Error starting terminal command thread, %i.\n", op_status);
					DataManager::SetValue("tw_terminal_state", 0);
					DataManager::SetValue("tw_background_thread_running", 0);
					operation_end(1, simulate);
				}
			}
			return 0;
		}
		if (function == "killterminal")
		{
			int op_status = 0;

			LOGINFO("Sending kill command...\n");
			operation_start("KillCommand");
			DataManager::SetValue("tw_operation_status", 0);
			DataManager::SetValue("tw_operation_state", 1);
			DataManager::SetValue("tw_terminal_state", 0);
			DataManager::SetValue("tw_background_thread_running", 0);
			DataManager::SetValue(TW_ACTION_BUSY, 0);
			return 0;
		}
		if (function == "reinjecttwrp")
		{
			int op_status = 0;
			string result;
			operation_start("ReinjectTWRP");
			gui_print("Injecting TWRP into boot image...\n");
			if (simulate) {
				simulate_progress_bar();
			} else {
				TWFunc::Exec_Cmd("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash");
				gui_print("TWRP injection complete.\n");
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "checkbackupname")
		{
			int op_status = 0;

			operation_start("CheckBackupName");
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = PartitionManager.Check_Backup_Name(true);
				if (op_status != 0)
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "decrypt")
		{
			int op_status = 0;

			operation_start("Decrypt");
			if (simulate) {
				simulate_progress_bar();
			} else {
				string Password;
				DataManager::GetValue("tw_crypto_password", Password);
				op_status = PartitionManager.Decrypt_Device(Password);
				if (op_status != 0)
					op_status = 1;
				else {
					int load_theme = 1;

					DataManager::SetValue(TW_IS_ENCRYPTED, 0);

					if (load_theme) {
						int has_datamedia;

						// Check for a custom theme and load it if exists
						DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
						if (has_datamedia != 0) {
							int storage_mounted = 0;
							std::string theme_path;
							std::string cmd;
							std::string base_xml;
							std::string mark_p;
							std::string mark_l;
							// Get the pre-selected theme
							base_xml = TWFunc::getUIxml(gr_get_rotation());
							theme_path = DataManager::GetStrValue(TW_SEL_THEME_PATH);
							if (theme_path.empty()) {
								// Built-in theme
								theme_path = base_xml;
							}
							// Mount storage
							if (!PartitionManager.Mount_Settings_Storage(false)) {
								LOGERR("Unable to mount storage during theme reload.\n");
								storage_mounted = 0;
								theme_path = base_xml;
							} else
								storage_mounted = 1;
		
							if (PageManager::ReloadPackage("TWRP", theme_path) != 0) {
								LOGERR("Failed to load base packages.\n");
								op_status = 1;
							} else {
								if (storage_mounted) {
									mark_p = DataManager::GetSettingsStoragePath() + "/TWRP/theme/.use_external_p";
									mark_l = DataManager::GetSettingsStoragePath() + "/TWRP/theme/.use_external_l";
									if (theme_path == base_xml)
										cmd = "rm -rf " + DataManager::GetSettingsStoragePath() + "/TWRP/theme/.use_external_*";
									else {
										if (gr_get_rotation() % 180 == 0) {
											if (TWFunc::Path_Exists(mark_l))
												unlink(mark_l.c_str());
											cmd = "echo " + theme_path + ">" + mark_p;
										} else {
											if (TWFunc::Path_Exists(mark_p))
												unlink(mark_p.c_str());
											cmd = "echo " + theme_path + ">" + mark_l;
										}
									}
									TWFunc::Exec_Cmd(cmd);
								}		
							}
						}
					}
				}
			}
			TWFunc::Vibrate((FeedbackReason)150);
			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "adbsideload")
		{
			int ret = 0;

			operation_start("Sideload");
			if (simulate) {
				simulate_progress_bar();
			} else {
				int wipe_cache = 0;
				int wipe_dalvik = 0;
				string result, Sideload_File;

				if (!PartitionManager.Mount_Current_Storage(true)) {
					operation_end(1, simulate);
					return 0;
				}
				Sideload_File = DataManager::GetCurrentStoragePath() + "/sideload.zip";
				if (TWFunc::Path_Exists(Sideload_File)) {
					unlink(Sideload_File.c_str());
				}
				gui_print("Starting ADB sideload feature...\n");
				DataManager::GetValue("tw_wipe_dalvik", wipe_dalvik);
				ret = apply_from_adb(Sideload_File.c_str());
				DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui now that the zip install is going to start
				if (ret != 0) {
					ret = 1; // failure
				} else if (TWinstall_zip(Sideload_File.c_str(), &wipe_cache) == 0) {
					if (wipe_cache || DataManager::GetIntValue("tw_wipe_cache"))
						PartitionManager.Wipe_By_Path("/cache");
					if (wipe_dalvik)
						PartitionManager.Wipe_Dalvik_Cache();
				} else {
					ret = 1; // failure
				}
				PartitionManager.Update_System_Details(true);
				if (DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) {
					operation_start("ReinjectTWRP");
					gui_print("Injecting TWRP into boot image...\n");
					if (simulate) {
						simulate_progress_bar();
					} else {
						string injectcmd = "";
						TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
						if (Boot == NULL || Boot->Current_File_System != "emmc")
							injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash";
						else
							injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash bd=" + Boot->Actual_Block_Device;
						TWFunc::Exec_Cmd(injectcmd);
						gui_print("TWRP injection complete.\n");
					}
				}
			}
			operation_end(ret, simulate);
			return 0;
		}
		if (function == "adbsideloadcancel")
		{
			int child_pid;
			char child_prop[PROPERTY_VALUE_MAX];
			string Sideload_File;
			Sideload_File = DataManager::GetCurrentStoragePath() + "/sideload.zip";
			unlink(Sideload_File.c_str());
			property_get("tw_child_pid", child_prop, "error");
			if (strcmp(child_prop, "error") == 0) {
				LOGERR("Unable to get child ID from prop\n");
				return 0;
			}
			child_pid = atoi(child_prop);
			gui_print("Cancelling ADB sideload...\n");
			kill(child_pid, SIGTERM);
			DataManager::SetValue("tw_page_done", "1"); // For OpenRecoveryScript support
			return 0;
		}
		if (function == "openrecoveryscript") {
			operation_start("OpenRecoveryScript");
			if (simulate) {
				simulate_progress_bar();
			} else {
				// Check for the SCRIPT_FILE_TMP first as these are AOSP recovery commands
				// that we converted to ORS commands during boot in recovery.cpp.
				// Run those first.
				int reboot = 0;
				if (TWFunc::Path_Exists(SCRIPT_FILE_TMP)) {
					gui_print("Processing AOSP recovery commands...\n");
					if (OpenRecoveryScript::run_script_file() == 0) {
						reboot = 1;
					}
				}
				// Check for the ORS file in /cache and attempt to run those commands.
				if (OpenRecoveryScript::check_for_script_file()) {
					gui_print("Processing OpenRecoveryScript file...\n");
					if (OpenRecoveryScript::run_script_file() == 0) {
						reboot = 1;
					}
				}
				if (reboot) {
					usleep(2000000); // Sleep for 2 seconds before rebooting
					TWFunc::tw_reboot(rb_system);
				} else {
					DataManager::SetValue("tw_page_done", 1);
				}
			}
			return 0;
		}
		if (function == "installsu") {
			int op_status = 0;

			operation_start("Install SuperSU");
			if (simulate) {
				simulate_progress_bar();
			} else {
				if (!TWFunc::Install_SuperSU())
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "fixsu") {
			int op_status = 0;

			operation_start("Fixing Superuser Permissions");
			if (simulate) {
				simulate_progress_bar();
			} else {
				LOGERR("Fixing su permissions was deprecated from TWRP.\n");
			        LOGERR("4.3+ ROMs with SELinux will always lose su perms.\n");
			}

			operation_end(op_status, simulate);
			return 0;
		}
#ifndef TW_EXCLUDE_ENCRYPTED_BACKUPS
		if (function == "decrypt_backup")
		{
			int op_status = 0;

			operation_start("Try Restore Decrypt");
			if (simulate) {
				simulate_progress_bar();
			} else {
				string Restore_Path, Filename, Password;
				DataManager::GetValue("tw_restore", Restore_Path);
				Restore_Path += "/";
				DataManager::GetValue("tw_restore_password", Password);
				if (TWFunc::Try_Decrypting_Backup(Restore_Path, Password))
					op_status = 0; // success
				else
					op_status = 1; // fail
			}

			operation_end(op_status, simulate);
			return 0;
		}
#endif
	} else {
		pthread_t t;
		pthread_create(&t, NULL, thread_start, this);
		return 0;
	}
	return -1;
}

int GUIAction::getKeyByName(std::string key)
{
	if (key == "home")		return KEY_HOME;
	else if (key == "menu")		return KEY_MENU;
	else if (key == "back")		return KEY_BACK;
	else if (key == "search")	return KEY_SEARCH;
	else if (key == "voldown")	return KEY_VOLUMEDOWN;
	else if (key == "volup")	return KEY_VOLUMEUP;
	else if (key == "power") {
		int ret_val;
		DataManager::GetValue(TW_POWER_BUTTON, ret_val);
		if (!ret_val)
			return KEY_POWER;
		else
			return ret_val;
	}

	return atol(key.c_str());
}

void* GUIAction::command_thread(void *cookie)
{
	string command;
	FILE* fp;
	char line[512];

	DataManager::GetValue("tw_terminal_command_thread", command);
	fp = popen(command.c_str(), "r");
	if (fp == NULL) {
		LOGERR("Error opening command to run.\n");
	} else {
		int fd = fileno(fp), has_data = 0, check = 0, keep_going = -1, bytes_read = 0;
		struct timeval timeout;
		fd_set fdset;

		while(keep_going) {
			FD_ZERO(&fdset);
			FD_SET(fd, &fdset);
			timeout.tv_sec = 0;
			timeout.tv_usec = 400000;
			has_data = select(fd+1, &fdset, NULL, NULL, &timeout);
			if (has_data == 0) {
				// Timeout reached
				DataManager::GetValue("tw_terminal_state", check);
				if (check == 0) {
					keep_going = 0;
				}
			} else if (has_data < 0) {
				// End of execution
				keep_going = 0;
			} else {
				// Try to read output
				memset(line, 0, sizeof(line));
				bytes_read = read(fd, line, sizeof(line));
				if (bytes_read > 0)
					gui_print("%s", line); // Display output
				else
					keep_going = 0; // Done executing
			}
		}
		fclose(fp);
	}
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue("tw_terminal_state", 0);
	DataManager::SetValue("tw_background_thread_running", 0);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
	return NULL;
}
