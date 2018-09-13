#include <functional>
#include "AsioUtil.h"

#pragma comment(lib,"User32.lib")
#pragma comment(lib,"Advapi32.lib")
#pragma comment(lib,"Ole32.lib")

#define ASIODRV_DESC		"description"
#define INPROC_SERVER		"InprocServer32"
#define ASIO_PATH			"software\\asio"
#define COM_CLSID			"clsid"

#define MAXDRVNAMELEN		128
#define MAXPATHLEN			512

#define CLEANUP(statement) Cleanup([&](){statement;})
namespace{
	using namespace std;
	struct CleanupList : public vector<function<void(void)>> {
		~CleanupList() {for(auto i(rbegin());i!=rend();++i) (*i)();}
		void operator()(function<void(void)> f) {push_back(f);}
	};
}

namespace ASIO{
	using namespace std;

	static LONG findDrvPath(char *clsidstr,BYTE *dllpath,int dllpathsize)
	{
		HKEY			hkEnum,hksub,hkpath;
		char			databuf[512];
		LONG 			cr,rc = -1;
		DWORD			datatype,datasize;
		DWORD			index;
		OFSTRUCT		ofs;
		HFILE			hfile;
		BOOL			found = FALSE;

		CleanupList Cleanup;

		CharLowerBuffA(clsidstr,(DWORD)strlen(clsidstr));
		if ((cr = RegOpenKeyA(HKEY_CLASSES_ROOT,COM_CLSID,&hkEnum)) == ERROR_SUCCESS) 
		{
			CLEANUP(RegCloseKey(hkEnum));
			index = 0;
			while (cr == ERROR_SUCCESS && !found) {
				cr = RegEnumKeyA(hkEnum,index++,databuf,512);
				if (cr == ERROR_SUCCESS) {
					CharLowerBuffA(databuf,(DWORD)strlen(databuf));
					if (!(strcmp(databuf,clsidstr))) {
						if ((cr = RegOpenKeyExA(hkEnum,(LPCSTR)databuf,0,KEY_READ,&hksub)) == ERROR_SUCCESS) {
							CLEANUP(RegCloseKey(hksub));
							if ((cr = RegOpenKeyExA(hksub,INPROC_SERVER,0,KEY_READ,&hkpath)) == ERROR_SUCCESS) {
								CLEANUP(RegCloseKey(hkpath));
								datatype = REG_SZ; datasize = (DWORD)dllpathsize;
								cr = RegQueryValueExA(hkpath,0,0,&datatype,(LPBYTE)dllpath,&datasize);
								if (cr == ERROR_SUCCESS) {
									memset(&ofs,0,sizeof(OFSTRUCT));
									ofs.cBytes = sizeof(OFSTRUCT); 
									hfile = OpenFile((LPCSTR)dllpath,&ofs,OF_EXIST);
									if (hfile) rc = 0; 
								}
							}
						}
						found = TRUE;	// break out 
					}
				}
			}				
		}
		return rc;
	}

	static void LoadDriver(HKEY hkey, string keyname, vector<DriverRecord>& drivers)
	{
		static const uint32_t _datasize(256);
		HKEY		hksub;
		BYTE		databuf[_datasize];
		BYTE		dllpath[MAXPATHLEN];
		DWORD		datatype, datasize = _datasize;

		CleanupList Cleanup;

		if (RegOpenKeyExA(hkey,keyname.c_str(),0,KEY_READ,&hksub) == ERROR_SUCCESS)
		{
			CLEANUP(RegCloseKey(hksub));

			datatype = REG_SZ;
			if (RegQueryValueExA(hksub,COM_CLSID,0,&datatype,databuf,&datasize) == ERROR_SUCCESS)
			{
				if (findDrvPath((char*)databuf,dllpath,MAXPATHLEN) == 0)
				{
					uint16_t	wData[100];
					MultiByteToWideChar(CP_ACP,0,(LPCSTR)databuf,-1,(LPWSTR)wData,100);

					DriverRecord asioDriver;

					CLSIDFromString((LPOLESTR)wData,(LPCLSID)&asioDriver.classID);

					datatype = REG_SZ;
					datasize = _datasize;

					if (RegQueryValueExA(hksub,ASIODRV_DESC,0,&datatype,(LPBYTE)databuf,&datasize) == ERROR_SUCCESS)
						asioDriver.driverName = std::string((const char*)databuf,(const char*)databuf+datasize-1);
					else
						asioDriver.driverName = keyname;

					try {
						COG::WinError::Context("ASIO driver instantiation");
						asioDriver.driverObject = COG::GetAndRetainObject<IASIO>(CoCreateInstance, asioDriver.classID, nullptr, CLSCTX_INPROC_SERVER, asioDriver.classID);
						drivers.emplace_back(std::move(asioDriver));
					} catch (std::exception &e) {
						std::string msg = "[ASIO] error " + asioDriver.driverName + ": " + e.what();
						OutputDebugStringA(msg.c_str());
					}
				}
			}
		}
	}

	std::vector<DriverRecord> GetDrivers()
	{
		CleanupList Cleanup;
		std::vector<DriverRecord> drivers;

		HKEY hkEnum = 0;
		char keyname[MAXDRVNAMELEN];
		DWORD index = 0;
		BOOL fin = FALSE;

		CLEANUP(if (hkEnum) RegCloseKey(hkEnum));

		for(auto cr = RegOpenKeyA(HKEY_LOCAL_MACHINE,ASIO_PATH,&hkEnum);
			cr == ERROR_SUCCESS;
			cr = RegEnumKeyA(hkEnum,index++,keyname,MAXDRVNAMELEN))
		{
			LoadDriver(hkEnum,keyname,drivers);
		}

		return drivers;
	}
}
