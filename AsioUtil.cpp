#include <functional>
#include "AsioUtil.h"

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

		CharLowerBuff(clsidstr,strlen(clsidstr));
		if ((cr = RegOpenKey(HKEY_CLASSES_ROOT,COM_CLSID,&hkEnum)) == ERROR_SUCCESS) 
		{
			CLEANUP(RegCloseKey(hkEnum));
			index = 0;
			while (cr == ERROR_SUCCESS && !found) {
				cr = RegEnumKey(hkEnum,index++,databuf,512);
				if (cr == ERROR_SUCCESS) {
					CharLowerBuff(databuf,strlen(databuf));
					if (!(strcmp(databuf,clsidstr))) {
						if ((cr = RegOpenKeyEx(hkEnum,(LPCTSTR)databuf,0,KEY_READ,&hksub)) == ERROR_SUCCESS) {
							CLEANUP(RegCloseKey(hksub));
							if ((cr = RegOpenKeyEx(hksub,(LPCTSTR)INPROC_SERVER,0,KEY_READ,&hkpath)) == ERROR_SUCCESS) {
								CLEANUP(RegCloseKey(hkpath));
								datatype = REG_SZ; datasize = (DWORD)dllpathsize;
								cr = RegQueryValueEx(hkpath,0,0,&datatype,(LPBYTE)dllpath,&datasize);
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

		if (RegOpenKeyEx(hkey,keyname.c_str(),0,KEY_READ,&hksub) == ERROR_SUCCESS)
		{
			CLEANUP(RegCloseKey(hksub));

			datatype = REG_SZ;
			if (RegQueryValueEx(hksub,COM_CLSID,0,&datatype,databuf,&datasize) == ERROR_SUCCESS)
			{
				if (findDrvPath((char*)databuf,dllpath,MAXPATHLEN) == 0)
				{
					uint16_t	wData[100];
					MultiByteToWideChar(CP_ACP,0,(LPCSTR)databuf,-1,(LPWSTR)wData,100);

					drivers.push_back(DriverRecord());
					auto &asioDriver(drivers.back());

					CLSIDFromString((LPOLESTR)wData,(LPCLSID)&asioDriver.classID);

					datatype = REG_SZ;
					datasize = _datasize;
					if (RegQueryValueEx(hksub,ASIODRV_DESC,0,&datatype,(LPBYTE)databuf,&datasize) == ERROR_SUCCESS)
						asioDriver.driverName = (const char*)databuf;
					else
						asioDriver.driverName = keyname;				
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

		for(auto cr = RegOpenKey(HKEY_LOCAL_MACHINE,ASIO_PATH,&hkEnum);
			cr == ERROR_SUCCESS;
			cr = RegEnumKey(hkEnum,index++,keyname,MAXDRVNAMELEN))
		{
			LoadDriver(hkEnum,keyname,drivers);
		}

		if (drivers.empty() == false) CoInitialize(0);
		return drivers;
	}

	ComRef<IASIO> DriverRecord::Load()
	{
		ComRef<IASIO> asio;
		if (CoCreateInstance(classID,0,CLSCTX_INPROC_SERVER,classID,(void**)asio.Placement()) == S_OK) return asio;
		else return ComRef<IASIO>();
	}
}