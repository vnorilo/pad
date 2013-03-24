PAD - Portable Audio Device

Modern C++ library for accessing audio devices. We plan to support WASAPI and ASIO on
Windows 7 and newer and CoreAudio on OS-X. Support for the old MME sound system
on Windows will not be included, therefore the library will not work on Windows XP. 
We have also decided that the code and the API is C++ (as far as possible), rather 
than C, to keep the API and code clean.

The C++(11) Standard Library facilities are used. We didn't want to create a yet another library 
that has rolled its own strings and dynamic arrays and and and...
If your compiler doesn't ship with a high performance and correct standard library and/or 
your work environment has a policy to not use the Standard Library, that is a problem 
we are unable to help you with.
