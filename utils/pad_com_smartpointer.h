#pragma once

// Helper stuff to deal with Microsoft's COM crap

#include "Objbase.h"
#include <string>
#include <vector>
#include <iostream>

template <class COMClass>
class PadComSmartPointer
{
public:
    PadComSmartPointer() throw() : m_ptr(0)
    {
    }
    PadComSmartPointer (COMClass* const obj) : m_ptr (obj)
    {
        if (m_ptr)
            m_ptr->AddRef();
    }
    PadComSmartPointer (const PadComSmartPointer<COMClass>& other) : m_ptr (other.m_ptr)
    {
        if (m_ptr) m_ptr->AddRef();
    }
    ~PadComSmartPointer()
    {
        release();
    }
    operator COMClass*() const throw()
    {
        return m_ptr;
    }
    COMClass& operator*() const throw()
    {
        return *m_ptr;
    }
    COMClass* operator->() const throw()
    {
        return m_ptr;
    }
    PadComSmartPointer& operator= (COMClass* const newPtr)
    {
        if (newPtr != 0)  newPtr->AddRef();
        release();
        m_ptr = newPtr;
        return *this;
    }

    PadComSmartPointer& operator= (const PadComSmartPointer<COMClass>& newPtr)
    {
        return operator= (newPtr.m_ptr);
    }

    HRESULT CoCreateInstance (REFCLSID classUUID, DWORD dwClsContext = CLSCTX_INPROC_SERVER)
    {
        HRESULT hr = ::CoCreateInstance (classUUID, 0, dwClsContext, __uuidof (COMClass), (void**) NullAndGetPtrAddress());
        return hr;
    }

    COMClass** NullAndGetPtrAddress()
    {
        release();
        m_ptr = 0;
        return &m_ptr;
    }

    template <class OtherComClass>
    HRESULT QueryInterface (REFCLSID classUUID, PadComSmartPointer<OtherComClass>& destObject) const
    {
        if (m_ptr == 0)
            return E_POINTER;

        return m_ptr->QueryInterface (classUUID, (void**) destObject.NullAndGetPtrAddress());
    }

    template <class OtherCOMClass>
    HRESULT QueryInterface (PadComSmartPointer<OtherCOMClass>& destObject) const
    {
        return this->QueryInterface (__uuidof (OtherCOMClass), destObject);
    }

private:
    COMClass* m_ptr;
    void release()
    {
        if (m_ptr!=0)
            m_ptr->Release();
    }
    COMClass** operator&() throw(); // to prevent ;D
};

class MyPropVariant
{
public:
    MyPropVariant()
    {
        PropVariantInit(&m_variant);
    }
    ~MyPropVariant()
    {
        //std::cerr << "MyPropVariant dtor\n";
        PropVariantClear(&m_variant);
    }
    MyPropVariant(const MyPropVariant& other)
    {
        //std::cerr << "MyPropVariant copy ctor\n";
        PropVariantCopy(&m_variant,&other.m_variant);
    }
    /* For completeness these probably should be implemented, but "meh" for now...
    bool operator==(const MyPropVariant & other) const
    {
        //obviously very bogus
        return true;
    }
    bool operator!=(const MyPropVariant & other)
    {
        return !(*this==other);
    }
    */
    MyPropVariant & operator= (const MyPropVariant & other)
    {
        //std::cerr << "MyPropVariant =\n";
        if (&this->m_variant!=&other.m_variant)
        {
            PropVariantClear(&m_variant);
            PropVariantCopy(&m_variant,&other.m_variant);
        }
        return *this;
    }
    PROPVARIANT* operator ()()
    {
        return &m_variant;
    }
private:
    PROPVARIANT m_variant;
};

// this might be slow, so before it is benchmarked, should avoid using in tight loops
static std::string WideCharToStdString(LPWSTR input)
{
    int sizeNeeded=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input,-1,0,0,NULL,NULL);
    if (sizeNeeded>0)
    {
        std::vector<char> buf(sizeNeeded,0);
        int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input,-1,buf.data(),sizeNeeded,NULL,NULL);
        if (size>0)
        {
            return std::string(buf.data());
        }
    }
    return std::string();
}

static bool CheckHResult(HRESULT r,const std::string& context=std::string())
{
    if (r<0)
    {
        if (context.size()>0)
            std::cerr << "COM/winapi error : "<<context<<" " << r << "\n";
        else std::cerr << "COM/winapi error : " << r << "\n";
        return false;
    }
    return true;
}

/* not really useful yet...this should wrap crap that needs to be deallocated with CoTaskMemFree etc
template <class T>
class COMPointer
{
public:
    COMPointer()
    {
        m_p=nullptr;
    }
    COMPointer(T* ptr)
    {
        m_p=ptr;
    }
    ~COMPointer()
    {
        if (m_p)
            CoTaskMemFree((LPVOID)m_p);
    }
    T* operator ()()
    {
        return &m_p;
    }
private:
    T* m_p;
};
*/
