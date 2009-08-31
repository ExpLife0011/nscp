/**************************************************************************
*   Copyright (C) 2004-2007 by Michael Medin <michael@medin.name>         *
*                                                                         *
*   This code is part of NSClient++ - http://trac.nakednuns.org/nscp      *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
***************************************************************************/
#include "stdafx.h"
#include "CheckDisk.h"
#include <strEx.h>
#include <time.h>
#include <filter_framework.hpp>
#include <error.hpp>
#include <file_helpers.hpp>

CheckDisk gCheckDisk;

BOOL APIENTRY DllMain( HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	NSCModuleWrapper::wrapDllMain(hModule, ul_reason_for_call);
	return TRUE;
}

CheckDisk::CheckDisk() {
}
CheckDisk::~CheckDisk() {
}

bool is_directory(DWORD dwAttr) {
	return ((dwAttr != INVALID_FILE_ATTRIBUTES) && ((dwAttr&FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY));
}

bool CheckDisk::loadModule() {
	try {
		NSCModuleHelper::registerCommand(_T("CheckFileSize"), _T("Check or directory a file and verify its size."));
		NSCModuleHelper::registerCommand(_T("CheckDriveSize"), _T("Check the size (free-space) of a drive or volume."));
		NSCModuleHelper::registerCommand(_T("CheckFile"), _T("Check various aspects of a file and/or folder."));
	} catch (NSCModuleHelper::NSCMHExcpetion &e) {
		NSC_LOG_ERROR_STD(_T("Failed to register command: ") + e.msg_);
	} catch (...) {
		NSC_LOG_ERROR_STD(_T("Failed to register command."));
	}
	return true;
}
bool CheckDisk::unloadModule() {
	return true;
}

bool CheckDisk::hasCommandHandler() {
	return true;
}
bool CheckDisk::hasMessageHandler() {
	return false;
}

class error_reporter {
public:
	virtual void report_error(std::wstring error) = 0;
	virtual void report_warning(std::wstring error) = 0;
};


struct file_finder_data {
	file_finder_data(const WIN32_FIND_DATA wfd_, const std::wstring path_, error_reporter *errors_) : wfd(wfd_), path(path_), errors(errors_) {}
	const WIN32_FIND_DATA wfd;
	const std::wstring path;
	error_reporter *errors;
};
typedef std::unary_function<const file_finder_data&, bool> baseFinderFunction;

struct get_size : public baseFinderFunction
{
	bool error;
	get_size() : size(0), error(false) { }
	result_type operator()(argument_type ffd) {
		if (!is_directory(ffd.wfd.dwFileAttributes)) {
			size += (ffd.wfd.nFileSizeHigh * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)ffd.wfd.nFileSizeLow;
		}
		return true;
	}
	inline unsigned long long getSize() {
		return size;
	}
	inline const bool hasError() const {
		return error;
	}
	inline void setError(error_reporter *errors, std::wstring msg) {
		if (errors != NULL)
			errors->report_error(msg);
		error = true;
	}
private:  
	unsigned long long size;
};

template <class finder_function>
void recursive_scan(std::wstring dir, std::wstring pattern, int current_level, int max_level, finder_function & f, error_reporter * errors) {
	if ((max_level != -1) && (current_level > max_level))
		return;
	WIN32_FIND_DATA wfd;

	DWORD fileAttr = GetFileAttributes(dir.c_str());
	NSC_DEBUG_MSG_STD(_T("Input is: ") + dir + _T(" / ") + strEx::ihextos(fileAttr));

	if (!is_directory(fileAttr)) {
		NSC_DEBUG_MSG_STD(_T("Found a file dont do recursive scan: ") + dir);
		// It is a file check it an return (dont check recursivly)
		pattern_type single_path = split_path_ex(dir);
		NSC_DEBUG_MSG_STD(_T("Path is: ") + single_path.first);
		HANDLE hFind = FindFirstFile(dir.c_str(), &wfd);
		if (hFind != INVALID_HANDLE_VALUE) {
			f(file_finder_data(wfd, single_path.first, errors));
			FindClose(hFind);
		}
		return;
	}
	std::wstring file_pattern = dir + _T("\\") + pattern;
	NSC_DEBUG_MSG_STD(_T("File pattern: ") + file_pattern);
	HANDLE hFind = FindFirstFile(file_pattern.c_str(), &wfd);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (!f(file_finder_data(wfd, dir, errors)))
				break;
		} while (FindNextFile(hFind, &wfd));
		FindClose(hFind);
	}
	std::wstring dir_pattern = dir + _T("\\*.*");
	NSC_DEBUG_MSG_STD(_T("File pattern: ") + dir_pattern);
	hFind = FindFirstFile(dir_pattern.c_str(), &wfd);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (is_directory(wfd.dwFileAttributes)) {
				if ( (wcscmp(wfd.cFileName, _T(".")) != 0) && (wcscmp(wfd.cFileName, _T("..")) != 0) )
					recursive_scan<finder_function>(dir + _T("\\") + wfd.cFileName, pattern, current_level+1, max_level, f, errors);
			}
		} while (FindNextFile(hFind, &wfd));
		FindClose(hFind);
	}
}



NSCAPI::nagiosReturn CheckDisk::CheckDriveSize(const unsigned int argLen, TCHAR **char_args, std::wstring &message, std::wstring &perf) {
	NSCAPI::nagiosReturn returnCode = NSCAPI::returnOK;
	std::list<std::wstring> args = arrayBuffer::arrayBuffer2list(argLen, char_args);
	if (args.empty()) {
		message = _T("Missing argument(s).");
		return NSCAPI::returnCRIT;
	}

	DriveContainer tmpObject;
	bool bFilter = false;
	bool bFilterRemote = false;
	bool bFilterRemovable = false;
	bool bFilterFixed = false;
	bool bFilterCDROM = false;
	bool bCheckAll = false;
	bool bCheckAllOthers = false;
	bool bNSClient = false;
	bool bPerfData = true;
	std::list<DriveContainer> drives;

	MAP_OPTIONS_BEGIN(args)
		MAP_OPTIONS_STR_AND(_T("Drive"), tmpObject.data, drives.push_back(tmpObject))
		MAP_OPTIONS_DISK_ALL(tmpObject, _T(""), _T("Free"), _T("Used"))
		MAP_OPTIONS_SHOWALL(tmpObject)
		MAP_OPTIONS_BOOL_VALUE(_T("FilterType"), bFilterFixed, _T("FIXED"))
		MAP_OPTIONS_BOOL_VALUE(_T("FilterType"), bFilterCDROM, _T("CDROM"))
		MAP_OPTIONS_BOOL_VALUE(_T("FilterType"), bFilterRemovable, _T("REMOVABLE"))
		MAP_OPTIONS_BOOL_VALUE(_T("FilterType"), bFilterRemote, _T("REMOTE"))
		MAP_OPTIONS_BOOL_FALSE(IGNORE_PERFDATA, bPerfData)
		MAP_OPTIONS_BOOL_TRUE(NSCLIENT, bNSClient)
		MAP_OPTIONS_BOOL_TRUE(CHECK_ALL, bCheckAll)
		MAP_OPTIONS_BOOL_TRUE(CHECK_ALL_OTHERS, bCheckAllOthers)
		MAP_OPTIONS_SECONDARY_BEGIN(_T(":"), p2)
			else if (p2.first == _T("Drive")) {
				tmpObject.data = p__.second;
				tmpObject.alias = p2.second;
				drives.push_back(tmpObject);
			}
		MAP_OPTIONS_MISSING_EX(p2, message, _T("Unknown argument: "))
		MAP_OPTIONS_SECONDARY_END()
	MAP_OPTIONS_FALLBACK_AND(tmpObject.data, drives.push_back(tmpObject))
	MAP_OPTIONS_END()
	bFilter = bFilterFixed || bFilterCDROM  || bFilterRemote || bFilterRemovable;

	if (drives.size() == 0)
		bCheckAll = true;

	if (bCheckAll) {
		DWORD dwDrives = GetLogicalDrives();
		int idx = 0;
		while (dwDrives != 0) {
			if (dwDrives & 0x1) {
				std::wstring drv;
				drv += static_cast<TCHAR>('A' + idx); drv += _T(":\\");
				UINT drvType = GetDriveType(drv.c_str());
				if ( ((!bFilter)&&(drvType == DRIVE_FIXED))  ||
					((bFilter)&&(bFilterFixed)&&(drvType==DRIVE_FIXED)) ||
					((bFilter)&&(bFilterCDROM)&&(drvType==DRIVE_CDROM)) ||
					((bFilter)&&(bFilterRemote)&&(drvType==DRIVE_REMOTE)) ||
					((bFilter)&&(bFilterRemovable)&&(drvType==DRIVE_REMOVABLE)) )
					drives.push_back(DriveContainer(drv, tmpObject.warn, tmpObject.crit));
			}
			idx++;
			dwDrives >>= 1;
		}
	}
	if (bCheckAllOthers) {
		std::list<DriveContainer> checkdrives;
		DWORD dwDrives = GetLogicalDrives();
		int idx = 0;
		while (dwDrives != 0) {
			if (dwDrives & 0x1) {
				std::wstring drv;
				drv += static_cast<TCHAR>('A' + idx); drv += _T(":\\");
				UINT drvType = GetDriveType(drv.c_str());
				if ( ((!bFilter)&&(drvType == DRIVE_FIXED))  ||
					((bFilter)&&(bFilterFixed)&&(drvType==DRIVE_FIXED)) ||
					((bFilter)&&(bFilterCDROM)&&(drvType==DRIVE_CDROM)) ||
					((bFilter)&&(bFilterRemote)&&(drvType==DRIVE_REMOTE)) ||
					((bFilter)&&(bFilterRemovable)&&(drvType==DRIVE_REMOVABLE)) )  
				{
					bool bFound = false;
					for (std::list<DriveContainer>::const_iterator pit = drives.begin();pit!=drives.end();++pit) {
						DriveContainer drive = (*pit);
						if (_wcsicmp(drive.data.substr(0,1).c_str(), drv.substr(0,1).c_str())==0)
							bFound = true;
					}
					if (!bFound)
						checkdrives.push_back(DriveContainer(drv, tmpObject.warn, tmpObject.crit));
				}
			}
			idx++;
			dwDrives >>= 1;
		}
		drives = checkdrives;
	}


	for (std::list<DriveContainer>::const_iterator pit = drives.begin();pit!=drives.end();++pit) {
		DriveContainer drive = (*pit);
		if (drive.data.length() == 1)
			drive.data += _T(":");
		drive.perfData = bPerfData;
		UINT drvType = GetDriveType(drive.data.c_str());

		if ((!bFilter)&&!((drvType == DRIVE_FIXED)||(drvType == DRIVE_NO_ROOT_DIR))) {
			message = _T("UNKNOWN: Drive is not a fixed drive: ") + drive.getAlias() + _T(" (it is a ") + get_filter(drvType) + _T(" drive)");
			return NSCAPI::returnUNKNOWN;
		} else if ( (bFilter)&&( (!bFilterFixed)&&((drvType==DRIVE_FIXED)||(drvType==DRIVE_NO_ROOT_DIR))) ||
			((!bFilterCDROM)&&(drvType==DRIVE_CDROM)) ||
			((!bFilterRemote)&&(drvType==DRIVE_REMOTE)) ||
			((!bFilterRemovable)&&(drvType==DRIVE_REMOVABLE)) ) {
				message = _T("UNKNOWN: Drive does not match the current filter: ") + drive.getAlias() + _T(" (it is a ") + get_filter(drvType) + _T(" drive)");
				return NSCAPI::returnUNKNOWN;
		}

		ULARGE_INTEGER freeBytesAvailableToCaller;
		ULARGE_INTEGER totalNumberOfBytes;
		ULARGE_INTEGER totalNumberOfFreeBytes;
		if (!GetDiskFreeSpaceEx(drive.data.c_str(), &freeBytesAvailableToCaller, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
			message = _T("UNKNOWN: Could not get free space for: ") + drive.getAlias() + _T(" ") + drive.data + _T(" reason: ") + error::lookup::last_error();
			return NSCAPI::returnUNKNOWN;
		}

		if (bNSClient) {
			if (!message.empty())
				message += _T("&");
			message += strEx::itos(totalNumberOfFreeBytes.QuadPart);
			message += _T("&");
			message += strEx::itos(totalNumberOfBytes.QuadPart);
		} else {
			checkHolders::PercentageValueType<checkHolders::disk_size_type, checkHolders::disk_size_type> value;
			std::wstring tstr;
			value.value = totalNumberOfBytes.QuadPart-totalNumberOfFreeBytes.QuadPart;
			value.total = totalNumberOfBytes.QuadPart;
			drive.setDefault(tmpObject);
			drive.runCheck(value, returnCode, message, perf);
		}
	}
	if (message.empty())
		message = _T("OK: All drives within bounds.");
	else if (!bNSClient)
		message = NSCHelper::translateReturn(returnCode) + _T(": ") + message;
	return returnCode;
}

std::wstring CheckDisk::get_filter(unsigned int drvType) {
	if (drvType==DRIVE_FIXED)
		return _T("fixed");
	if (drvType==DRIVE_NO_ROOT_DIR)
		return _T("no_root");
	if (drvType==DRIVE_CDROM)
		return _T("cdrom");
	if (drvType==DRIVE_REMOTE)
		return _T("remote");
	if (drvType==DRIVE_REMOVABLE)
		return _T("removable");
	return _T("unknown: ") + strEx::itos(drvType);
}

class NSC_error : public error_reporter {
	void report_error(std::wstring error) {
		NSC_LOG_ERROR(error);
	}
	void report_warning(std::wstring error) {
		NSC_LOG_MESSAGE(error);
	}
};
typedef std::pair<std::wstring,std::wstring> pattern_type;
pattern_type split_path_ex(std::wstring path) {
	std::wstring baseDir;
	if (file_helpers::checks::is_directory(path)) {
		return pattern_type(path, _T(""));
	}
	std::wstring::size_type pos = path.find_last_of('\\');
	if (pos == std::wstring::npos) {
		pattern_type(path, _T("*.*"));
	}
	NSC_DEBUG_MSG_STD(_T("Looking for: path: ") + path.substr(0, pos) + _T(", pattern: ") + path.substr(pos+1));
	return pattern_type(path.substr(0, pos), path.substr(pos+1));
}

typedef std::pair<std::wstring,std::wstring> pattern_type;
pattern_type split_pattern(std::wstring path) {
	std::wstring baseDir;
	if (file_helpers::checks::exists(path)) {
		return pattern_type(path, _T(""));
	}
	std::wstring::size_type pos = path.find_last_of('\\');
	if (pos == std::wstring::npos) {
		pattern_type(path, _T("*.*"));
	}
	NSC_DEBUG_MSG_STD(_T("Looking for: pattern: ") + path.substr(0, pos) + _T(", pattern: ") + path.substr(pos+1));
	return pattern_type(path.substr(0, pos), path.substr(pos+1));
}


NSCAPI::nagiosReturn CheckDisk::CheckFileSize(const unsigned int argLen, TCHAR **char_args, std::wstring &message, std::wstring &perf) {
	NSCAPI::nagiosReturn returnCode = NSCAPI::returnOK;
	std::list<std::wstring> args = arrayBuffer::arrayBuffer2list(argLen, char_args);
	bool bPerfData = true;
	if (args.empty()) {
		message = _T("Missing argument(s).");
		return NSCAPI::returnUNKNOWN;
	}
	PathContainer tmpObject;
	std::list<PathContainer> paths;

	MAP_OPTIONS_BEGIN(args)
		MAP_OPTIONS_STR_AND(_T("File"), tmpObject.data, paths.push_back(tmpObject))
		MAP_OPTIONS_SHOWALL(tmpObject)
		MAP_OPTIONS_STR(_T("MaxWarn"), tmpObject.warn.max)
		MAP_OPTIONS_STR(_T("MinWarn"), tmpObject.warn.min)
		MAP_OPTIONS_STR(_T("MaxCrit"), tmpObject.crit.max)
		MAP_OPTIONS_STR(_T("MinCrit"), tmpObject.crit.min)
		MAP_OPTIONS_BOOL_FALSE(IGNORE_PERFDATA, bPerfData)
		MAP_OPTIONS_SECONDARY_BEGIN(_T(":"), p2)
		else if (p2.first == _T("File")) {
			tmpObject.data = p__.second;
			tmpObject.alias = p2.second;
			paths.push_back(tmpObject);
		}
		MAP_OPTIONS_MISSING_EX(p2, message, _T("Unknown argument: "))
		MAP_OPTIONS_SECONDARY_END()
		MAP_OPTIONS_MISSING(message, _T("Unknown argument: "))
	MAP_OPTIONS_END()

	for (std::list<PathContainer>::const_iterator pit = paths.begin(); pit != paths.end(); ++pit) {
		PathContainer path = (*pit);
		std::wstring tstr;
		std::wstring sName = path.getAlias();
		get_size sizeFinder;
		NSC_error errors;
		pattern_type splitpath = split_pattern(path.data);
		recursive_scan<get_size>(splitpath.first, splitpath.second, -1, -1, sizeFinder, &errors);
		if (sizeFinder.hasError()) {
			message = _T("File not found check log for details");
			return NSCAPI::returnUNKNOWN;
		}
		path.setDefault(tmpObject);
		path.perfData = bPerfData;

		checkHolders::disk_size_type size = sizeFinder.getSize();
		path.runCheck(size, returnCode, message, perf);
	}
	if (message.empty())
		message = _T("OK all file sizes are within bounds.");
	else
		message = NSCHelper::translateReturn(returnCode) + _T(": ") + message;
	return returnCode;
}


struct file_info {
	file_info() : ullCreationTime(0) {}
	file_info(const BY_HANDLE_FILE_INFORMATION info, std::wstring filename_) : filename(filename_), ullCreationTime(0) {
		ullSize = ((info.nFileSizeHigh * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)info.nFileSizeLow);
		ullCreationTime = ((info.ftCreationTime.dwHighDateTime * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)info.ftCreationTime.dwLowDateTime);
		ullLastAccessTime = ((info.ftLastAccessTime.dwHighDateTime * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)info.ftLastAccessTime.dwLowDateTime);
		ullLastWriteTime = ((info.ftLastWriteTime.dwHighDateTime * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)info.ftLastWriteTime.dwLowDateTime);
	};

	unsigned long long ullSize;
	unsigned long long ullCreationTime;
	unsigned long long ullLastAccessTime;
	unsigned long long ullLastWriteTime;
	unsigned long long ullNow;
	std::wstring filename;

	std::wstring render(std::wstring syntax) {
		strEx::replace(syntax, _T("%filename%"), filename);
		strEx::replace(syntax, _T("%creation%"), strEx::format_filetime(ullCreationTime, DATE_FORMAT));
		strEx::replace(syntax, _T("%access%"), strEx::format_filetime(ullLastAccessTime, DATE_FORMAT));
		strEx::replace(syntax, _T("%write%"), strEx::format_filetime(ullLastWriteTime, DATE_FORMAT));
		strEx::replace(syntax, _T("%size%"), strEx::itos_as_BKMG(ullSize));
		return syntax;
	}

};

struct file_filter {
	filters::filter_all_numeric<unsigned long long, checkHolders::disk_size_handler<checkHolders::disk_size_type> > size;
	filters::filter_all_times creation;
	filters::filter_all_times accessed;
	filters::filter_all_times written;
	static const __int64 MSECS_TO_100NS = 10000;

	inline bool hasFilter() {
		return size.hasFilter() || creation.hasFilter() || 
			accessed.hasFilter() || written.hasFilter();
	}
	bool matchFilter(const file_info &value) const {
		if ((size.hasFilter())&&(size.matchFilter(value.ullSize)))
			return true;
		else if ((creation.hasFilter())&&(creation.matchFilter((value.ullNow-value.ullCreationTime)/MSECS_TO_100NS)))
			return true;
		else if ((accessed.hasFilter())&&(accessed.matchFilter((value.ullNow-value.ullLastAccessTime)/MSECS_TO_100NS)))
			return true;
		else if ((written.hasFilter())&&(written.matchFilter((value.ullNow-value.ullLastWriteTime)/MSECS_TO_100NS)))
			return true;
		return false;
	}

	std::wstring getValue() const {
		if (size.hasFilter())
			return _T("size: ") + size.getValue();
		if (creation.hasFilter())
			return _T("creation: ") + creation.getValue();
		if (accessed.hasFilter())
			return _T("accessed: ") + accessed.getValue();
		if (written.hasFilter())
			return _T("written: ") + written.getValue();
		return _T("UNknown...");
	}

};


struct find_first_file_info : public baseFinderFunction
{
	file_info info;
	bool error;
//	std::wstring message;
	find_first_file_info() : error(false) {}
	result_type operator()(argument_type ffd) {
		if (is_directory(ffd.wfd.dwFileAttributes))
			return true;
		BY_HANDLE_FILE_INFORMATION _info;

		HANDLE hFile = CreateFile((ffd.path + _T("\\") + ffd.wfd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
			0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
		if (hFile == INVALID_HANDLE_VALUE) {
			setError(ffd.errors, _T("Could not open file: ") + ffd.path + _T("\\") + ffd.wfd.cFileName + _T(": ") + error::lookup::last_error());
			return false;
		}
		GetFileInformationByHandle(hFile, &_info);
		CloseHandle(hFile);
		info = file_info(_info, ffd.wfd.cFileName);
		return false;
	}
	inline const bool hasError() const {
		return error;
	}
	inline void setError(error_reporter *errors, std::wstring msg) {
		if (errors != NULL)
			errors->report_error(msg);
		error = true;
	}
};

struct file_filter_function : public baseFinderFunction
{
	std::list<file_filter> filter_chain;
	bool bFilterAll;
	bool bFilterIn;
	bool error;
	std::wstring message;
	std::wstring syntax;
	std::wstring alias;
	unsigned long long now;
	unsigned int hit_count;

	file_filter_function() : hit_count(0), error(false), bFilterIn(true), bFilterAll(true) {}
	result_type operator()(argument_type ffd) {
		if (is_directory(ffd.wfd.dwFileAttributes))
			return true;
		BY_HANDLE_FILE_INFORMATION _info;

		HANDLE hFile = CreateFile((ffd.path + _T("\\") + ffd.wfd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
			0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
		if (hFile == INVALID_HANDLE_VALUE) {
			setError(ffd.errors, _T("Could not open file: ") + ffd.path + _T("\\") + ffd.wfd.cFileName + _T(": ") + error::lookup::last_error());
			return true;
		}
		GetFileInformationByHandle(hFile, &_info);
		CloseHandle(hFile);
		file_info info(_info, ffd.wfd.cFileName);
		info.ullNow = now;

		for (std::list<file_filter>::const_iterator cit3 = filter_chain.begin(); cit3 != filter_chain.end(); ++cit3 ) {
			bool bMatch = bFilterAll;
			bool bTmpMatched = (*cit3).matchFilter(info);
			if (bFilterAll) {
				if (!bTmpMatched) {
					bMatch = false;
					break;
				}
			} else {
				if (bTmpMatched) {
					bMatch = true;
					break;
				}
			}
			if ((bFilterIn&&bMatch)||(!bFilterIn&&!bMatch)) {
				strEx::append_list(message, info.render(syntax));
				if (alias.length() < 16)
					strEx::append_list(alias, info.filename);
				else
					strEx::append_list(alias, std::wstring(_T("...")));
				hit_count++;
			}
		}
		return true;
	}
	inline const bool hasError() const {
		return error;
	}
	inline void setError(error_reporter *errors, std::wstring msg) {
		if (errors != NULL)
			errors->report_error(msg);
		error = true;
	}
};



struct file_filter_function_ex : public baseFinderFunction
{
	static const int filter_plus = 1;
	static const int filter_minus = 2;
	static const int filter_normal = 3;

	typedef std::pair<int,file_filter> filteritem_type;
	typedef std::list<filteritem_type > filterlist_type;
	filterlist_type filter_chain;
	bool bFilterAll;
	bool bFilterIn;
	bool error;
	bool debug_;
	std::wstring message;
	std::wstring syntax;
	std::wstring alias;
	unsigned long long now;
	unsigned int hit_count;

	file_filter_function_ex() : hit_count(0), error(false), debug_(false), bFilterIn(true), bFilterAll(true) {}
	result_type operator()(argument_type ffd) {
		if (is_directory(ffd.wfd.dwFileAttributes))
			return true;
		BY_HANDLE_FILE_INFORMATION _info;

		HANDLE hFile = CreateFile((ffd.path + _T("\\") + ffd.wfd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
			0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
		if (hFile == INVALID_HANDLE_VALUE) {
			setError(ffd.errors, _T("Could not open file: ") + ffd.path + _T("\\") + ffd.wfd.cFileName + _T(": ") + error::lookup::last_error());
			return true;
		}
		GetFileInformationByHandle(hFile, &_info);
		CloseHandle(hFile);
		file_info info(_info, ffd.wfd.cFileName);
		info.ullNow = now;

		bool bMatch = !bFilterIn;
		for (filterlist_type::const_iterator cit3 = filter_chain.begin(); cit3 != filter_chain.end(); ++cit3 ) {
			bool bTmpMatched = (*cit3).second.matchFilter(info);
			int mode = (*cit3).first;

			if ((mode == filter_minus)&&(bTmpMatched)) {
				// a -<filter> hit so thrash item and bail out!
				//if (debug_)
					NSC_DEBUG_MSG_STD(_T("Matched: - ") + (*cit3).second.getValue() + _T(" for: ") + info.render(syntax));
				bMatch = false;
				break;
			} else if ((mode == filter_plus)&&(!bTmpMatched)) {
				// a +<filter> missed hit so thrash item and bail out!
				//if (debug_)
					NSC_DEBUG_MSG_STD(_T("Matched (missed): + ") + (*cit3).second.getValue() + _T(" for: ") + info.render(syntax));
				bMatch = false;
				break;
			} else if (bTmpMatched) {
				if (debug_)
					NSC_DEBUG_MSG_STD(_T("Matched: . (contiunue): ") + (*cit3).second.getValue() + _T(" for: ") + info.render(syntax));
				bMatch = true;
			}
		}

		NSC_DEBUG_MSG_STD(_T("result: ") + strEx::itos(bFilterIn) + _T(" -- ") + strEx::itos(bMatch));
		if ((bFilterIn&&bMatch)||(!bFilterIn&&!bMatch)) {
			strEx::append_list(message, info.render(syntax));
			if (alias.length() < 16)
				strEx::append_list(alias, info.filename);
			else
				strEx::append_list(alias, std::wstring(_T("...")));
			hit_count++;
		}
		return true;
	}
	inline const bool hasError() const {
		return error;
	}
	inline void setError(error_reporter *errors, std::wstring msg) {
		if (errors != NULL)
			errors->report_error(msg);
		error = true;
	}
};


NSCAPI::nagiosReturn CheckDisk::getFileAge(const unsigned int argLen, TCHAR **char_args, std::wstring &message, std::wstring &perf) {
	NSCAPI::nagiosReturn returnCode = NSCAPI::returnOK;
	std::list<std::wstring> stl_args = arrayBuffer::arrayBuffer2list(argLen, char_args);
	typedef checkHolders::CheckContainer<checkHolders::MaxMinBoundsUInteger> CheckFileContainer;
	if (stl_args.empty()) {
		message = _T("Missing argument(s).");
		return NSCAPI::returnUNKNOWN;
	}
	std::wstring format = _T("%Y years %m mon %d days %H hours %M min %S sec");
	std::wstring path;
	find_first_file_info finder;
	MAP_OPTIONS_BEGIN(stl_args)
		MAP_OPTIONS_STR(_T("path"), path)
		MAP_OPTIONS_STR(_T("date"), format)
		MAP_OPTIONS_FALLBACK(format)
	MAP_OPTIONS_END()

	if (path.empty()) {
		message = _T("ERROR: no file specified.");
		return NSCAPI::returnUNKNOWN;
	}

	NSC_error errors;
	pattern_type splitpath = split_pattern(path);
	recursive_scan<find_first_file_info>(splitpath.first, splitpath.second, -1, -1, finder, &errors);
	if (finder.hasError()) {
		message = _T("File not found (check log for details)");
		return NSCAPI::returnUNKNOWN;
	}
	FILETIME now_;
	GetSystemTimeAsFileTime(&now_);
	unsigned long long now = ((now_.dwHighDateTime * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)now_.dwLowDateTime);
	time_t value = (now-finder.info.ullLastWriteTime)/10000000;
	message = strEx::itos(value/60) + _T("&") + strEx::format_time_delta(gmtime(&value), format);
	return NSCAPI::returnOK;
}


NSCAPI::nagiosReturn CheckDisk::CheckFile(const unsigned int argLen, TCHAR **char_args, std::wstring &message, std::wstring &perf) {
	NSCAPI::nagiosReturn returnCode = NSCAPI::returnOK;
	std::list<std::wstring> stl_args = arrayBuffer::arrayBuffer2list(argLen, char_args);
	typedef checkHolders::CheckContainer<checkHolders::MaxMinBoundsUInteger> CheckFileContainer;
	if (stl_args.empty()) {
		message = _T("Missing argument(s).");
		return NSCAPI::returnUNKNOWN;
	}
	file_filter_function finder;
	PathContainer tmpObject;
	std::list<std::wstring> paths;
	unsigned int truncate = 0;
	CheckFileContainer query;
	std::wstring syntax = _T("%filename%");
	std::wstring alias;
	bool bPerfData = true;
	unsigned int max_dir_depth = -1;

	try {
		MAP_OPTIONS_BEGIN(stl_args)
			MAP_OPTIONS_NUMERIC_ALL(query, _T(""))
			MAP_OPTIONS_STR2INT(_T("truncate"), truncate)
			MAP_OPTIONS_BOOL_FALSE(IGNORE_PERFDATA, bPerfData)
			MAP_OPTIONS_STR(_T("syntax"), syntax)
			MAP_OPTIONS_PUSH(_T("path"), paths)
			MAP_OPTIONS_STR(_T("alias"), alias)
			MAP_OPTIONS_STR2INT(_T("max-dir-depth"), max_dir_depth)
			MAP_OPTIONS_PUSH(_T("file"), paths)
			MAP_OPTIONS_BOOL_EX(_T("filter"), finder.bFilterIn, _T("in"), _T("out"))
			MAP_OPTIONS_BOOL_EX(_T("filter"), finder.bFilterAll, _T("all"), _T("any"))
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-size"), size, finder.filter_chain)
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-creation"), creation, finder.filter_chain)
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-written"), written, finder.filter_chain)
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-accessed"), accessed, finder.filter_chain)
			MAP_OPTIONS_MISSING(message, _T("Unknown argument: "))
			MAP_OPTIONS_END()
	} catch (filters::parse_exception e) {
		message = e.getMessage();
		return NSCAPI::returnUNKNOWN;
	} catch (filters::filter_exception e) {
		message = e.getMessage();
		return NSCAPI::returnUNKNOWN;
	}
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	finder.now = ((now.dwHighDateTime * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)now.dwLowDateTime);
	finder.syntax = syntax;
	NSC_error errors;
	for (std::list<std::wstring>::const_iterator pit = paths.begin(); pit != paths.end(); ++pit) {
		pattern_type path = split_pattern(*pit);
		recursive_scan<file_filter_function>(path.first, path.second, 0, max_dir_depth, finder, &errors);
		if (finder.hasError()) {
			message = _T("File not found: ") + (*pit) + _T(" check log for details.");
			return NSCAPI::returnUNKNOWN;
		}
	}
	message = finder.message;
	if (finder.error)
		return NSCAPI::returnUNKNOWN;
	if (!alias.empty())
		query.alias = alias;
	else
		query.alias = finder.alias;
	if (query.alias.empty())
		query.alias = _T("no files found");
	query.runCheck(finder.hit_count, returnCode, message, perf);
	if ((truncate > 0) && (message.length() > (truncate-4)))
		message = message.substr(0, truncate-4) + _T("...");
	if (message.empty())
		message = _T("CheckFile ok");
	return returnCode;
}

#define MAP_FILTER(value, obj, filtermode) \
			else if (p__.first == value) { file_filter filter; filter.obj = p__.second; finder.filter_chain.push_back(filteritem_type(file_filter_function_ex::filtermode, filter)); }

NSCAPI::nagiosReturn CheckDisk::CheckFile2(const unsigned int argLen, TCHAR **char_args, std::wstring &message, std::wstring &perf) {
	NSCAPI::nagiosReturn returnCode = NSCAPI::returnOK;
	std::list<std::wstring> stl_args = arrayBuffer::arrayBuffer2list(argLen, char_args);
	typedef checkHolders::CheckContainer<checkHolders::MaxMinBoundsUInteger> CheckFileContainer;
	typedef std::pair<int,file_filter> filteritem_type;
	typedef std::list<filteritem_type > filterlist_type;
	if (stl_args.empty()) {
		message = _T("Missing argument(s).");
		return NSCAPI::returnUNKNOWN;
	}
	file_filter_function_ex finder;
	PathContainer tmpObject;
	std::list<std::wstring> paths;
	unsigned int truncate = 0;
	CheckFileContainer query;
	std::wstring syntax = _T("%filename%");
	std::wstring alias;
	std::wstring pattern = _T("*.*");
	bool bPerfData = true;
	int max_dir_depth = -1;

	try {
		MAP_OPTIONS_BEGIN(stl_args)
			MAP_OPTIONS_NUMERIC_ALL(query, _T(""))
			MAP_OPTIONS_STR2INT(_T("truncate"), truncate)
			MAP_OPTIONS_BOOL_FALSE(IGNORE_PERFDATA, bPerfData)
			MAP_OPTIONS_STR(_T("syntax"), syntax)
			MAP_OPTIONS_PUSH(_T("path"), paths)
			MAP_OPTIONS_STR(_T("pattern"), pattern)
			MAP_OPTIONS_STR(_T("alias"), alias)
			MAP_OPTIONS_PUSH(_T("file"), paths)
			MAP_OPTIONS_STR2INT(_T("max-dir-depth"), max_dir_depth)
			MAP_OPTIONS_BOOL_EX(_T("filter"), finder.bFilterIn, _T("in"), _T("out"))
			MAP_OPTIONS_BOOL_EX(_T("filter"), finder.bFilterAll, _T("all"), _T("any"))
			/*
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-size"), fileSize, finder.filter_chain)
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-creation"), fileCreation, finder.filter_chain)
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-written"), fileWritten, finder.filter_chain)
			MAP_OPTIONS_PUSH_WTYPE(file_filter, _T("filter-accessed"), fileAccessed, finder.filter_chain)
			*/

			MAP_FILTER(_T("filter+size"), size, filter_plus)
			MAP_FILTER(_T("filter+creation"), creation, filter_plus)
			MAP_FILTER(_T("filter+written"), written, filter_plus)
			MAP_FILTER(_T("filter+accessed"), accessed, filter_plus)

			MAP_FILTER(_T("filter.size"), size, filter_normal)
			MAP_FILTER(_T("filter.creation"), creation, filter_normal)
			MAP_FILTER(_T("filter.written"), written, filter_normal)
			MAP_FILTER(_T("filter.accessed"), accessed, filter_normal)

			MAP_FILTER(_T("filter-size"), size, filter_minus)
			MAP_FILTER(_T("filter-creation"), creation, filter_minus)
			MAP_FILTER(_T("filter-written"), written, filter_minus)
			MAP_FILTER(_T("filter-accessed"), accessed, filter_minus)

			MAP_OPTIONS_MISSING(message, _T("Unknown argument: "))
			MAP_OPTIONS_END()
	} catch (filters::parse_exception e) {
		message = e.getMessage();
		return NSCAPI::returnUNKNOWN;
	} catch (filters::filter_exception e) {
		message = e.getMessage();
		return NSCAPI::returnUNKNOWN;
		}
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		finder.now = ((now.dwHighDateTime * ((unsigned long long)MAXDWORD+1)) + (unsigned long long)now.dwLowDateTime);
		finder.syntax = syntax;
		NSC_error errors;
		for (std::list<std::wstring>::const_iterator pit = paths.begin(); pit != paths.end(); ++pit) {
			recursive_scan<file_filter_function_ex>(*pit, pattern, 0, max_dir_depth, finder, &errors);
			if (finder.hasError()) {
				message = _T("Error when scanning: ") + (*pit) + _T(" check log for details.");
				return NSCAPI::returnUNKNOWN;
			}
		}
		message = finder.message;
		if (!alias.empty())
			query.alias = alias;
		else
			query.alias = finder.alias;
		if (query.alias.empty())
			query.alias = _T("no files found");
		query.runCheck(finder.hit_count, returnCode, message, perf);
		if ((truncate > 0) && (message.length() > (truncate-4)))
			message = message.substr(0, truncate-4) + _T("...");
		if (message.empty())
			message = _T("CheckFile ok");
		return returnCode;
}

NSCAPI::nagiosReturn CheckDisk::handleCommand(const strEx::blindstr command, const unsigned int argLen, TCHAR **char_args, std::wstring &msg, std::wstring &perf) {
	if (command == _T("CheckFileSize")) {
		return CheckFileSize(argLen, char_args, msg, perf);
	} else if (command == _T("CheckDriveSize")) {
		return CheckDriveSize(argLen, char_args, msg, perf);
	} else if (command == _T("CheckFile")) {
		return CheckFile(argLen, char_args, msg, perf);
	} else if (command == _T("CheckFile2")) {
		return CheckFile2(argLen, char_args, msg, perf);
	} else if (command == _T("getFileAge")) {
		return getFileAge(argLen, char_args, msg, perf);
	}	
	return NSCAPI::returnIgnored;
}


NSC_WRAPPERS_MAIN_DEF(gCheckDisk);
NSC_WRAPPERS_IGNORE_MSG_DEF();
NSC_WRAPPERS_HANDLE_CMD_DEF(gCheckDisk);
