#include <ostream>
#include <WinBase.h>

class windbgbuf : public std::streambuf {
public:
	int overflow(int c) {
		if (c != traits_type::eof()) {
			char ch[2] = { traits_type::to_char_type(c), 0 };
			OutputDebugStringA(ch);
		}
		return std::streambuf::overflow(c);
	}

	std::streamsize xsputn(const char *buffer, std::streamsize n) {
		for (int i = 0; i < n; ++i) {
			char ch[2] = { buffer[i], 0 };
			OutputDebugStringA(ch);
		}
		return n;
	}
};

class cwindbg : public std::ostream {
	windbgbuf buf;
public:
	cwindbg():std::ostream(&buf) { }
};



