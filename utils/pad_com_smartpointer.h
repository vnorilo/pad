#pragma once

#include "Objbase.h"

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
        HRESULT hr = ::CoCreateInstance (classUUID, 0, dwClsContext, __uuidof (COMClass), (void**) resetAndGetPointerAddress());
        return hr;
    }

    COMClass** resetAndGetPointerAddress()
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

        return m_ptr->QueryInterface (classUUID, (void**) destObject.resetAndGetPointerAddress());
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
