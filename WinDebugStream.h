#include <ostream>
#include <string>
#include <WinBase.h>

class windbgbuf : public std::streambuf {
public:
	int overflow(int c) {
		if (c != traits_type::eof()) {
			char ch[2] = { traits_type::to_char_type(c), 0 };
			OutputDebugStringA(ch);
		}
		return c;
	}

	std::streamsize xsputn(const char *buffer, std::streamsize n) {
		std::string buf(buffer, buffer + n);
		OutputDebugStringA(buf.c_str());
		return n;
	}
};

class cwindbg : public std::ostream {
	windbgbuf buf;
public:
	cwindbg():std::ostream(&buf) { }
};



