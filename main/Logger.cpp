#include "stdafx.h"
#include "Logger.h"
#include <iostream>
#include <stdarg.h>
#include <time.h>
#include <algorithm>
#include "localtime_r.h"
#include "Helper.h"
#include "mainworker.h"

#ifndef WIN32
#include <syslog.h>
#include <errno.h>
#endif

#include "SQLHelper.h"

#define MAX_LOG_LINE_BUFFER 100
#define MAX_LOG_LINE_LENGTH (2048 * 3)

#define MAX_ACLFLOG_LINES 100000

extern bool g_bRunAsDaemon;
extern bool g_bUseSyslog;

CLogger::_tLogLineStruct::_tLogLineStruct(const _eLogLevel nlevel, const std::string &nlogmessage)
{
	logtime = mytime(nullptr);
	level = nlevel;
	logmessage = nlogmessage;
}

CLogger::CLogger()
{
	m_bInSequenceMode = false;
	m_bEnableLogThreadIDs = false;
	m_bEnableLogTimestamps = true;
	m_bEnableErrorsToNotificationSystem = false;
	m_LastLogNotificationsSend = 0;
	SetLogFlags(LOG_NORM | LOG_STATUS | LOG_ERROR);
	SetDebugFlags(DEBUG_NORM);
}

CLogger::~CLogger()
{
	if (m_outputfile.is_open())
		m_outputfile.close();
}

// Supported flags: all,normal,status,error,debug
bool CLogger::SetLogFlags(const std::string &sFlags)
{
	std::vector<std::string> flags;
	StringSplit(sFlags, ",", flags);

	uint32_t iFlags = 0;

	for (auto &wflag : flags)
	{
		stdstring_trim(wflag);
		if (wflag.empty())
			continue;
		if (is_number(wflag))
		{
			// Flags are set provided (bitwise)
			iFlags = strtoul(wflag.c_str(), nullptr, 10);
			break;
		}
		if (wflag == "all")
			iFlags |= LOG_ALL;
		else if (wflag == "normal")
			iFlags |= LOG_NORM;
		else if (wflag == "status")
			iFlags |= LOG_STATUS;
		else if (wflag == "error")
			iFlags |= LOG_ERROR;
		else if (wflag == "debug")
			iFlags |= LOG_DEBUG_INT;
		else
			continue; // invalid flag, skip but continue processing the other flags
	}
	if (iFlags == 0)
		iFlags = LOG_STATUS + LOG_ERROR;
	SetLogFlags(iFlags);
	return true;
}

void CLogger::SetLogFlags(const uint32_t iFlags)
{
	m_log_flags = iFlags;
}

// Supported flags: all,normal,hardware,received,webserver,eventsystem,python,thread_id,sql,auth
bool CLogger::SetDebugFlags(const std::string &sFlags)
{
	std::vector<std::string> flags;
	StringSplit(sFlags, ",", flags);

	uint32_t iFlags = 0;

	for (auto &wflag : flags)
	{
		stdstring_trim(wflag);
		if (wflag.empty())
			continue;
		if (is_number(wflag))
		{
			// Flags are set provided (bitwise)
			iFlags = strtoul(wflag.c_str(), nullptr, 10);
			break;
		}
		if (wflag == "all")
			iFlags |= DEBUG_ALL;
		else if (wflag == "normal")
			iFlags |= DEBUG_NORM;
		else if (wflag == "hardware")
			iFlags |= DEBUG_HARDWARE;
		else if (wflag == "received")
			iFlags |= DEBUG_RECEIVED;
		else if (wflag == "webserver")
			iFlags |= DEBUG_WEBSERVER;
		else if (wflag == "eventsystem")
			iFlags |= DEBUG_EVENTSYSTEM;
		else if (wflag == "python")
			iFlags |= DEBUG_PYTHON;
		else if (wflag == "thread_id")
			iFlags |= DEBUG_THREADIDS;
		else if (wflag == "sql")
			iFlags |= DEBUG_SQL;
		else if (wflag == "auth")
			iFlags |= DEBUG_AUTH;
		else
			continue; // invalid flag, skip but continue processing the other flags
	}
	SetDebugFlags(iFlags);
	if(IsDebugLevelEnabled(DEBUG_WEBSERVER))
		SetACLFlogFlags(LOG_ACLF_ENABLED);
	if (iFlags && !IsLogLevelEnabled(LOG_DEBUG_INT))
	{
		m_log_flags |= LOG_DEBUG_INT;
		Log(LOG_STATUS, "Enabling Debug logging!");
	}
	return true;
}

void CLogger::SetDebugFlags(const uint32_t iFlags)
{
	m_debug_flags = iFlags;
}

void CLogger::SetACLFlogFlags(const uint8_t iFlags)
{
	m_aclf_flags |= iFlags;
}

bool CLogger::IsLogLevelEnabled(const _eLogLevel level)
{
	return (m_log_flags & level);
}

bool CLogger::IsDebugLevelEnabled(const _eDebugLevel level)
{
	if (!(m_log_flags & LOG_DEBUG_INT))
		return false;
	return (m_debug_flags & level);
}

bool CLogger::IsACLFlogEnabled()
{
	if (!(m_aclf_flags & LOG_ACLF_ENABLED))
		return false;
	return true;
}

void CLogger::SetOutputFile(const char *OutputFile)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	if (m_outputfile.is_open())
		m_outputfile.close();

	if (OutputFile == nullptr)
		return;
	if (*OutputFile == 0)
		return;

	try
	{
#ifdef _DEBUG
		m_outputfile.open(OutputFile, std::ios::out | std::ios::trunc);
#else
		m_outputfile.open(OutputFile, std::ios::out | std::ios::app);
#endif
	}
	catch (...)
	{
		std::cerr << "Error opening output log file..." << std::endl;
	}
}

void CLogger::SetACLFOutputFile(const char *OutputFile)
{
	std::string sLogFile = OutputFile;

	if(sLogFile.find("syslog:") != std::string::npos)
	{
		Log(LOG_STATUS, "Weblogs are send to SYSLOG!");
		SetACLFlogFlags(LOG_ACLF_SYSLOG);
	}
	else
	{
		m_aclflogfile = OutputFile;
		SetACLFlogFlags(LOG_ACLF_FILE);
	}
	SetACLFlogFlags(LOG_ACLF_ENABLED);
}

void CLogger::OpenACLFOutputFile()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	if (m_aclfoutputfile.is_open())
		m_aclfoutputfile.close();

	if (m_aclflogfile == nullptr)
		return;
	if (*m_aclflogfile == 0)
		return;

	try
	{
#ifdef _DEBUG
		m_aclfoutputfile.open(m_aclflogfile, std::ios::out | std::ios::trunc);
#else
		m_aclfoutputfile.open(m_aclflogfile, std::ios::out | std::ios::app);
#endif
	}
	catch (...)
	{
		std::cerr << "Error opening Apache Combined LogFormat webserver log file..." << std::endl;
	}
}

void CLogger::ForwardErrorsToNotificationSystem(const bool bDoForward)
{
	m_bEnableErrorsToNotificationSystem = bDoForward;
	if (!bDoForward)
		m_notification_log.clear();
}

void CLogger::Log(const _eLogLevel level, const std::string &sLogline)
{
	Log(level, "%s", sLogline.c_str());
}

void CLogger::Log(const _eLogLevel level, const char *logline, ...)
{
	if (!(m_log_flags & level))
		return; // This log level is not enabled!

	va_list argList;
	char cbuffer[MAX_LOG_LINE_LENGTH];
	va_start(argList, logline);
	vsnprintf(cbuffer, sizeof(cbuffer), logline, argList);
	va_end(argList);

#ifndef WIN32
	if (g_bUseSyslog)
	{
		int sLogLevel = LOG_INFO;
		if (level & LOG_ERROR)
			sLogLevel = LOG_ERR;
		else if (level & LOG_STATUS)
			sLogLevel = LOG_NOTICE;
		syslog(sLogLevel, "%s", cbuffer);
	}
#endif

	std::stringstream sstr;

	if (m_bEnableLogTimestamps)
		sstr << TimeToString(nullptr, TF_DateTimeMs) << "  ";

	if ((m_log_flags & LOG_DEBUG_INT) && (m_debug_flags & DEBUG_THREADIDS))
	{
#ifdef WIN32
		sstr << "[" << std::setfill('0') << std::setw(4) << std::hex << ::GetCurrentThreadId() << "] ";
#else
		sstr << "[" << std::setfill('0') << std::setw(4) << std::hex << pthread_self() << "] ";
#endif
	}

	if (level & LOG_STATUS)
		sstr << "Status: " << cbuffer;
	else if (level & LOG_ERROR)
		sstr << "Error: " << cbuffer;
	else if (level & LOG_DEBUG_INT)
		sstr << "Debug: " << cbuffer;
	else
		sstr << cbuffer;

	std::string szIntLog = sstr.str();

	{
		// Locked region to allow multiple threads to print at the same time
		std::unique_lock<std::mutex> lock(m_mutex);

		if ((level & LOG_ERROR) && (m_bEnableErrorsToNotificationSystem))
		{
			if (m_notification_log.size() >= MAX_LOG_LINE_BUFFER)
				m_notification_log.erase(m_notification_log.begin());
			m_notification_log.push_back(_tLogLineStruct(level, szIntLog));
			if ((m_notification_log.size() == 1) && (mytime(nullptr) - m_LastLogNotificationsSend >= 5))
			{
				m_mainworker.ForceLogNotificationCheck();
			}
		}

		if (!g_bRunAsDaemon)
		{
			// output to console
#ifndef WIN32
			if (level != LOG_ERROR)
#endif
				std::cout << szIntLog << std::endl;
#ifndef WIN32
			else // print text in red color
				std::cout << szIntLog.substr(0, 25) << "\033[1;31m" << szIntLog.substr(25) << "\033[0;0m" << std::endl;
#endif
		}

		if (m_outputfile.is_open())
		{
			// output to file
			m_outputfile << szIntLog << std::endl;
			m_outputfile.flush();
		}

		auto itt = m_lastlog.find(level);
		if (itt != m_lastlog.end())
		{
			if (m_lastlog[level].size() >= MAX_LOG_LINE_BUFFER)
				m_lastlog[level].erase(m_lastlog[level].begin());
		}
		m_lastlog[level].push_back(_tLogLineStruct(level, szIntLog));
	}
}

void CLogger::Debug(const _eDebugLevel level, const char *logline, ...)
{
	if (!IsDebugLevelEnabled(level))
		return;
	va_list argList;
	char cbuffer[MAX_LOG_LINE_LENGTH];
	va_start(argList, logline);
	vsnprintf(cbuffer, sizeof(cbuffer), logline, argList);
	va_end(argList);
	Debug(level, std::string(cbuffer));
}

void CLogger::Debug(const _eDebugLevel level, const std::string &sLogline)
{
	if (!IsDebugLevelEnabled(level))
		return;
	Log(LOG_DEBUG_INT, sLogline);
}

void CLogger::ACLFlog(const char *logline, ...)
{
	if (!IsACLFlogEnabled())
		return;
	va_list argList;
	char cbuffer[MAX_LOG_LINE_LENGTH];
	va_start(argList, logline);
	vsnprintf(cbuffer, sizeof(cbuffer), logline, argList);
	va_end(argList);

	if(IsDebugLevelEnabled(DEBUG_WEBSERVER))
	{
		//std::cout << std::string(cbuffer) << std::endl;
		Debug(DEBUG_WEBSERVER,"Web ACLF: %s", cbuffer);
	}

	if(m_aclf_flags & LOG_ACLF_FILE)
	{
		if(m_aclf_loggedlinescnt++ >= MAX_ACLFLOG_LINES || (!m_aclfoutputfile.is_open()))
		{
			if(m_aclfoutputfile.is_open())
				m_aclfoutputfile.close();
			OpenACLFOutputFile();
			m_aclf_loggedlinescnt = 1;
		}
		if (m_aclfoutputfile.is_open())
		{
			// output to file
			m_aclfoutputfile << std::string(cbuffer) << std::endl;
			m_aclfoutputfile.flush();
		}
	}

#ifndef WIN32
	if(g_bUseSyslog && (m_aclf_flags & LOG_ACLF_SYSLOG))
	{
		syslog(LOG_INFO|LOG_LOCAL1,"%s", cbuffer);
	}
#endif
}

bool strhasEnding(std::string const &fullString, std::string const &ending)
{
	return fullString.size() >= ending.size() && !fullString.compare(fullString.size() - ending.size(), ending.size(), ending);
}

void CLogger::LogSequenceStart()
{
	m_bInSequenceMode = true;
	m_sequencestring.clear();
	m_sequencestring.str("");
}

void CLogger::LogSequenceEnd(const _eLogLevel level)
{
	if (!m_bInSequenceMode)
		return;

	std::string message = m_sequencestring.str();
	if (strhasEnding(message, "\n"))
	{
		message = message.substr(0, message.size() - 1);
	}

	Log(level, message);
	m_sequencestring.clear();
	m_sequencestring.str("");

	m_bInSequenceMode = false;
}

void CLogger::LogSequenceAdd(const char *logline)
{
	if (!m_bInSequenceMode)
		return;

	m_sequencestring << logline << std::endl;
}

void CLogger::LogSequenceAddNoLF(const char *logline)
{
	if (!m_bInSequenceMode)
		return;

	m_sequencestring << logline;
}

void CLogger::EnableLogTimestamps(const bool bEnableTimestamps)
{
	m_bEnableLogTimestamps = bEnableTimestamps;
}

bool CLogger::IsLogTimestampsEnabled()
{
	return (m_bEnableLogTimestamps && !g_bUseSyslog);
}

bool compareLogByTime(const CLogger::_tLogLineStruct &a, CLogger::_tLogLineStruct &b)
{
	return a.logtime < b.logtime;
}

std::list<CLogger::_tLogLineStruct> CLogger::GetLog(const _eLogLevel level, const time_t lastlogtime)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	std::list<_tLogLineStruct> mlist;

	if (level != LOG_ALL)
	{
		if (m_lastlog.find(level) == m_lastlog.end())
			return mlist;

		std::copy_if(std::begin(m_lastlog[level]), std::end(m_lastlog[level]), std::back_inserter(mlist), [lastlogtime](const _tLogLineStruct &l) { return l.logtime > lastlogtime; });
	}
	else
		for (const auto &l : m_lastlog)
			std::copy_if(l.second.begin(), l.second.end(), std::back_inserter(mlist), [lastlogtime](const _tLogLineStruct &l2) { return l2.logtime > lastlogtime; });

	// Sort by time
	mlist.sort(compareLogByTime);
	return mlist;
}

void CLogger::ClearLog()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_lastlog.clear();
}

std::list<CLogger::_tLogLineStruct> CLogger::GetNotificationLogs()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	std::list<_tLogLineStruct> mlist;
	std::copy(m_notification_log.begin(), m_notification_log.end(), std::back_inserter(mlist));
	m_notification_log.clear();
	if (!mlist.empty())
		m_LastLogNotificationsSend = mytime(nullptr);
	return mlist;
}

bool CLogger::NotificationLogsEnabled()
{
	return m_bEnableErrorsToNotificationSystem;
}
