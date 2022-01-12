#pragma once

#include <stdexcept>
#include <string>

namespace PAD{
	enum ErrorCode {
		NoError,
		InternalError,
		ChannelRangeInvalid,
		ChannelRangeOverlap,
		UnknownApiIdentifier,
		DeviceStartStreamFailure,
		DeviceOpenStreamFailure,
		DeviceInitializationFailure,
		DeviceDriverFailure,
		DeviceDeinitializationFailure,
		DeviceCloseStreamFailure,
		DeviceStopStreamFailure
	};

	class Error : public std::runtime_error {
		ErrorCode code;
	protected:
		Error(ErrorCode c,const std::string& message):code(c),runtime_error(message.c_str()) {}
	public:
		ErrorCode GetCode() const {return code;}
		virtual bool IsHard() const { return false; }
	};

	/**
	* throw only SoftError and HardError 
	***/
	class SoftError : public Error {
	public:
		SoftError(ErrorCode c, const std::string& message):Error(c,message){}
	};

	class HardError : public Error {
	public:
		HardError(ErrorCode c, const std::string& message):Error(c,message){}
		virtual bool IsHard() const { return true; }
	};

}
