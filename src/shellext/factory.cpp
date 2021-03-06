
#include "factory.h"
#include "menu.h"
#include <new>
#include <Shlwapi.h>
#pragma comment(lib, "shlwapi.lib")


extern long g_DllRef;


ClassFactory::ClassFactory() :
    m_RefCount(1)
{
    InterlockedIncrement(&g_DllRef);
}


ClassFactory::~ClassFactory()
{
    InterlockedDecrement(&g_DllRef);
}


//
// IUnknown
//

IFACEMETHODIMP ClassFactory::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] = 
    {
        QITABENT(ClassFactory, IClassFactory),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}


IFACEMETHODIMP_(ULONG) ClassFactory::AddRef()
{
    return InterlockedIncrement(&m_RefCount);
}


IFACEMETHODIMP_(ULONG) ClassFactory::Release()
{
    ULONG cRef = InterlockedDecrement(&m_RefCount);
    if (0 == cRef)
    {
        delete this;
    }
    return cRef;
}


// 
// IClassFactory
//

IFACEMETHODIMP ClassFactory::CreateInstance(IUnknown *outer, REFIID riid, void **ppv)
{
    HRESULT hr = CLASS_E_NOAGGREGATION;

    // outer is used for aggregation.
    if (outer == NULL)
    {
        hr = E_OUTOFMEMORY;

        // Create the COM component.
        ComposerShellMenu *handler = new (std::nothrow) ComposerShellMenu();
        if (handler)
        {
            // Query the specified interface.
            hr = handler->QueryInterface(riid, ppv);
            handler->Release();
        }
    }

    return hr;
}


IFACEMETHODIMP ClassFactory::LockServer(BOOL fLock)
{
    if (fLock)
    {
        InterlockedIncrement(&g_DllRef);
    }
    else
    {
        InterlockedDecrement(&g_DllRef);
    }
    return S_OK;
}
