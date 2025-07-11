/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  A dataset and raster band classes that differ the opening of the
 *           underlying dataset in a limited pool of opened datasets.
 * Author:   Even Rouault <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal_proxy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_hash_set.h"
#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "gdal.h"
#include "gdal_priv.h"

//! @cond Doxygen_Suppress

/* We *must* share the same mutex as the gdaldataset.cpp file, as we are */
/* doing GDALOpen() calls that can indirectly call GDALOpenShared() on */
/* an auxiliary dataset ... */
/* Then we could get dead-locks in multi-threaded use case */

/* ******************************************************************** */
/*                         GDALDatasetPool                              */
/* ******************************************************************** */

/* This class is a singleton that maintains a pool of opened datasets */
/* The cache uses a LRU strategy */

class GDALDatasetPool;
static GDALDatasetPool *singleton = nullptr;

void GDALNullifyProxyPoolSingleton()
{
    singleton = nullptr;
}

struct _GDALProxyPoolCacheEntry
{
    GIntBig responsiblePID;
    char *pszFileNameAndOpenOptions;
    char *pszOwner;
    GDALDataset *poDS;
    GIntBig nRAMUsage;

    /* Ref count of the cached dataset */
    int refCount;

    GDALProxyPoolCacheEntry *prev;
    GDALProxyPoolCacheEntry *next;
};

// This variable prevents a dataset that is going to be opened in
// GDALDatasetPool::_RefDataset from increasing refCount if, during its
// opening, it creates a GDALProxyPoolDataset.
// We increment it before opening or closing a cached dataset and decrement
// it afterwards
// The typical use case is a VRT made of simple sources that are VRT
// We don't want the "inner" VRT to take a reference on the pool, otherwise
// there is a high chance that this reference will not be dropped and the pool
// remain ghost.
static thread_local int refCountOfDisabledRefCount = 0;

class GDALDatasetPool
{
  private:
    bool bInDestruction = false;

    /* Ref count of the pool singleton */
    /* Taken by "toplevel" GDALProxyPoolDataset in its constructor and released
     */
    /* in its destructor. See also refCountOfDisabledRefCount for the difference
     */
    /* between toplevel and inner GDALProxyPoolDataset */
    int refCount = 0;

    int maxSize = 0;
    int currentSize = 0;
    int64_t nMaxRAMUsage = 0;
    int64_t nRAMUsage = 0;
    GDALProxyPoolCacheEntry *firstEntry = nullptr;
    GDALProxyPoolCacheEntry *lastEntry = nullptr;

    /* Caution : to be sure that we don't run out of entries, size must be at */
    /* least greater or equal than the maximum number of threads */
    explicit GDALDatasetPool(int maxSize, int64_t nMaxRAMUsage);
    ~GDALDatasetPool();
    GDALProxyPoolCacheEntry *_RefDataset(const char *pszFileName,
                                         GDALAccess eAccess,
                                         CSLConstList papszOpenOptions,
                                         int bShared, bool bForceOpen,
                                         const char *pszOwner);
    void _CloseDatasetIfZeroRefCount(const char *pszFileName,
                                     CSLConstList papszOpenOptions,
                                     GDALAccess eAccess, const char *pszOwner);

#ifdef DEBUG_PROXY_POOL
    // cppcheck-suppress unusedPrivateFunction
    void ShowContent();
    void CheckLinks();
#endif

    CPL_DISALLOW_COPY_ASSIGN(GDALDatasetPool)

  public:
    static void Ref();
    static void Unref();
    static GDALProxyPoolCacheEntry *RefDataset(const char *pszFileName,
                                               GDALAccess eAccess,
                                               char **papszOpenOptions,
                                               int bShared, bool bForceOpen,
                                               const char *pszOwner);
    static void UnrefDataset(GDALProxyPoolCacheEntry *cacheEntry);
    static void CloseDatasetIfZeroRefCount(const char *pszFileName,
                                           CSLConstList papszOpenOptions,
                                           GDALAccess eAccess,
                                           const char *pszOwner);

    static void PreventDestroy();
    static void ForceDestroy();
};

/************************************************************************/
/*                         GDALDatasetPool()                            */
/************************************************************************/

GDALDatasetPool::GDALDatasetPool(int maxSizeIn, int64_t nMaxRAMUsageIn)
    : maxSize(maxSizeIn), nMaxRAMUsage(nMaxRAMUsageIn)
{
}

/************************************************************************/
/*                        ~GDALDatasetPool()                            */
/************************************************************************/

GDALDatasetPool::~GDALDatasetPool()
{
    bInDestruction = true;
    GDALProxyPoolCacheEntry *cur = firstEntry;
    GIntBig responsiblePID = GDALGetResponsiblePIDForCurrentThread();
    while (cur)
    {
        GDALProxyPoolCacheEntry *next = cur->next;
        CPLFree(cur->pszFileNameAndOpenOptions);
        CPLFree(cur->pszOwner);
        CPLAssert(cur->refCount == 0);
        if (cur->poDS)
        {
            GDALSetResponsiblePIDForCurrentThread(cur->responsiblePID);
            GDALClose(cur->poDS);
        }
        CPLFree(cur);
        cur = next;
    }
    GDALSetResponsiblePIDForCurrentThread(responsiblePID);
}

#ifdef DEBUG_PROXY_POOL
/************************************************************************/
/*                            ShowContent()                             */
/************************************************************************/

void GDALDatasetPool::ShowContent()
{
    GDALProxyPoolCacheEntry *cur = firstEntry;
    int i = 0;
    while (cur)
    {
        printf("[%d] pszFileName=%s, owner=%s, refCount=%d, " /*ok*/
               "responsiblePID=%d\n",
               i,
               cur->pszFileNameAndOpenOptions ? cur->pszFileNameAndOpenOptions
                                              : "(null)",
               cur->pszOwner ? cur->pszOwner : "(null)", cur->refCount,
               (int)cur->responsiblePID);
        i++;
        cur = cur->next;
    }
}

/************************************************************************/
/*                             CheckLinks()                             */
/************************************************************************/

void GDALDatasetPool::CheckLinks()
{
    GDALProxyPoolCacheEntry *cur = firstEntry;
    int i = 0;
    while (cur)
    {
        CPLAssert(cur == firstEntry || cur->prev->next == cur);
        CPLAssert(cur == lastEntry || cur->next->prev == cur);
        ++i;
        CPLAssert(cur->next != nullptr || cur == lastEntry);
        cur = cur->next;
    }
    (void)i;
    CPLAssert(i == currentSize);
}
#endif

/************************************************************************/
/*                       GetFilenameAndOpenOptions()                    */
/************************************************************************/

static std::string GetFilenameAndOpenOptions(const char *pszFileName,
                                             CSLConstList papszOpenOptions)
{
    std::string osFilenameAndOO(pszFileName);
    for (int i = 0; papszOpenOptions && papszOpenOptions[i]; ++i)
    {
        osFilenameAndOO += "||";
        osFilenameAndOO += papszOpenOptions[i];
    }
    return osFilenameAndOO;
}

/************************************************************************/
/*                            _RefDataset()                             */
/************************************************************************/

GDALProxyPoolCacheEntry *
GDALDatasetPool::_RefDataset(const char *pszFileName, GDALAccess eAccess,
                             CSLConstList papszOpenOptions, int bShared,
                             bool bForceOpen, const char *pszOwner)
{
    CPLMutex **pMutex = GDALGetphDLMutex();
    CPLMutexHolderD(pMutex);

    if (bInDestruction)
        return nullptr;

    const GIntBig responsiblePID = GDALGetResponsiblePIDForCurrentThread();

    const auto EvictEntryWithZeroRefCount =
        [this, responsiblePID](bool evictEntryWithOpenedDataset)
    {
        GDALProxyPoolCacheEntry *cur = firstEntry;
        GDALProxyPoolCacheEntry *candidate = nullptr;
        while (cur)
        {
            GDALProxyPoolCacheEntry *next = cur->next;

            if (cur->refCount == 0 &&
                (!evictEntryWithOpenedDataset || cur->nRAMUsage > 0))
            {
                candidate = cur;
            }

            cur = next;
        }
        if (candidate == nullptr)
            return false;

        nRAMUsage -= candidate->nRAMUsage;
        candidate->nRAMUsage = 0;

        CPLFree(candidate->pszFileNameAndOpenOptions);
        candidate->pszFileNameAndOpenOptions = nullptr;

        if (candidate->poDS)
        {
            /* Close by pretending we are the thread that GDALOpen'ed this */
            /* dataset */
            GDALSetResponsiblePIDForCurrentThread(candidate->responsiblePID);

            refCountOfDisabledRefCount++;
            GDALClose(candidate->poDS);
            refCountOfDisabledRefCount--;

            candidate->poDS = nullptr;
            GDALSetResponsiblePIDForCurrentThread(responsiblePID);
        }
        CPLFree(candidate->pszOwner);
        candidate->pszOwner = nullptr;

        if (!evictEntryWithOpenedDataset && candidate != firstEntry)
        {
            /* Recycle this entry for the to-be-opened dataset and */
            /* moves it to the top of the list */
            if (candidate->prev)
                candidate->prev->next = candidate->next;

            if (candidate->next)
                candidate->next->prev = candidate->prev;
            else
            {
                CPLAssert(candidate == lastEntry);
                lastEntry->prev->next = nullptr;
                lastEntry = lastEntry->prev;
            }
            candidate->prev = nullptr;
            candidate->next = firstEntry;
            firstEntry->prev = candidate;
            firstEntry = candidate;

#ifdef DEBUG_PROXY_POOL
            CheckLinks();
#endif
        }

        return true;
    };

    GDALProxyPoolCacheEntry *cur = firstEntry;

    const std::string osFilenameAndOO =
        GetFilenameAndOpenOptions(pszFileName, papszOpenOptions);

    while (cur)
    {
        GDALProxyPoolCacheEntry *next = cur->next;

        if (cur->refCount >= 0 && cur->pszFileNameAndOpenOptions &&
            osFilenameAndOO == cur->pszFileNameAndOpenOptions &&
            ((bShared && cur->responsiblePID == responsiblePID &&
              ((cur->pszOwner == nullptr && pszOwner == nullptr) ||
               (cur->pszOwner != nullptr && pszOwner != nullptr &&
                strcmp(cur->pszOwner, pszOwner) == 0))) ||
             (!bShared && cur->refCount == 0)))
        {
            if (cur != firstEntry)
            {
                /* Move to begin */
                if (cur->next)
                    cur->next->prev = cur->prev;
                else
                    lastEntry = cur->prev;
                cur->prev->next = cur->next;
                cur->prev = nullptr;
                firstEntry->prev = cur;
                cur->next = firstEntry;
                firstEntry = cur;

#ifdef DEBUG_PROXY_POOL
                CheckLinks();
#endif
            }

            cur->refCount++;
            return cur;
        }

        cur = next;
    }

    if (!bForceOpen)
        return nullptr;

    if (currentSize == maxSize)
    {
        if (!EvictEntryWithZeroRefCount(false))
        {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "Too many threads are running for the current value of the "
                "dataset pool size (%d).\n"
                "or too many proxy datasets are opened in a cascaded way.\n"
                "Try increasing GDAL_MAX_DATASET_POOL_SIZE.",
                maxSize);
            return nullptr;
        }

        CPLAssert(firstEntry);
        cur = firstEntry;
    }
    else
    {
        /* Prepend */
        cur = static_cast<GDALProxyPoolCacheEntry *>(
            CPLCalloc(1, sizeof(GDALProxyPoolCacheEntry)));
        if (lastEntry == nullptr)
            lastEntry = cur;
        cur->prev = nullptr;
        cur->next = firstEntry;
        if (firstEntry)
            firstEntry->prev = cur;
        firstEntry = cur;
        currentSize++;
#ifdef DEBUG_PROXY_POOL
        CheckLinks();
#endif
    }

    cur->pszFileNameAndOpenOptions = CPLStrdup(osFilenameAndOO.c_str());
    cur->pszOwner = (pszOwner) ? CPLStrdup(pszOwner) : nullptr;
    cur->responsiblePID = responsiblePID;
    cur->refCount = -1;  // to mark loading of dataset in progress
    cur->nRAMUsage = 0;

    refCountOfDisabledRefCount++;
    const int nFlag =
        ((eAccess == GA_Update) ? GDAL_OF_UPDATE : GDAL_OF_READONLY) |
        GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR;
    CPLConfigOptionSetter oSetter("CPL_ALLOW_VSISTDIN", "NO", true);

    // Release mutex while opening dataset to avoid lock contention.
    CPLReleaseMutex(*pMutex);
    auto poDS = GDALDataset::Open(pszFileName, nFlag, nullptr, papszOpenOptions,
                                  nullptr);
    CPLAcquireMutex(*pMutex, 1000.0);

    cur->poDS = poDS;
    cur->refCount = 1;

    refCountOfDisabledRefCount--;

    if (cur->poDS)
    {
        cur->nRAMUsage =
            std::max<GIntBig>(0, cur->poDS->GetEstimatedRAMUsage());
        nRAMUsage += cur->nRAMUsage;
    }

    if (nMaxRAMUsage > 0 && cur->nRAMUsage > 0)
    {
        while (nRAMUsage > nMaxRAMUsage && nRAMUsage != cur->nRAMUsage &&
               EvictEntryWithZeroRefCount(true))
        {
            // ok
        }
    }

    return cur;
}

/************************************************************************/
/*                   _CloseDatasetIfZeroRefCount()                      */
/************************************************************************/

void GDALDatasetPool::_CloseDatasetIfZeroRefCount(const char *pszFileName,
                                                  CSLConstList papszOpenOptions,
                                                  GDALAccess /* eAccess */,
                                                  const char *pszOwner)
{
    // May fix https://github.com/OSGeo/gdal/issues/4318
    if (bInDestruction)
        return;

    GDALProxyPoolCacheEntry *cur = firstEntry;
    GIntBig responsiblePID = GDALGetResponsiblePIDForCurrentThread();

    const std::string osFilenameAndOO =
        GetFilenameAndOpenOptions(pszFileName, papszOpenOptions);

    while (cur)
    {
        GDALProxyPoolCacheEntry *next = cur->next;

        if (cur->refCount == 0 && cur->pszFileNameAndOpenOptions &&
            osFilenameAndOO == cur->pszFileNameAndOpenOptions &&
            ((pszOwner == nullptr && cur->pszOwner == nullptr) ||
             (pszOwner != nullptr && cur->pszOwner != nullptr &&
              strcmp(cur->pszOwner, pszOwner) == 0)) &&
            cur->poDS != nullptr)
        {
            /* Close by pretending we are the thread that GDALOpen'ed this */
            /* dataset */
            GDALSetResponsiblePIDForCurrentThread(cur->responsiblePID);

            GDALDataset *poDS = cur->poDS;

            nRAMUsage -= cur->nRAMUsage;
            cur->nRAMUsage = 0;

            cur->poDS = nullptr;
            CPLFree(cur->pszFileNameAndOpenOptions);
            cur->pszFileNameAndOpenOptions = nullptr;
            CPLFree(cur->pszOwner);
            cur->pszOwner = nullptr;

            refCountOfDisabledRefCount++;
            GDALClose(poDS);
            refCountOfDisabledRefCount--;

            GDALSetResponsiblePIDForCurrentThread(responsiblePID);
            break;
        }

        cur = next;
    }
}

/************************************************************************/
/*                       GDALGetMaxDatasetPoolSize()                    */
/************************************************************************/

/** Return the maximum number of datasets simultaneously opened in the
 * dataset pool.
 */
int GDALGetMaxDatasetPoolSize()
{
    int nSize = atoi(CPLGetConfigOption("GDAL_MAX_DATASET_POOL_SIZE", "100"));
    if (nSize < 2)
        nSize = 2;
    else if (nSize > 1000)
        nSize = 1000;
    return nSize;
}

/************************************************************************/
/*                                 Ref()                                */
/************************************************************************/

void GDALDatasetPool::Ref()
{
    CPLMutexHolderD(GDALGetphDLMutex());
    if (singleton == nullptr)
    {

        // Try to not consume more than 25% of the usable RAM
        GIntBig l_nMaxRAMUsage =
            (CPLGetUsablePhysicalRAM() - GDALGetCacheMax64()) / 4;
        const char *pszMaxRAMUsage =
            CPLGetConfigOption("GDAL_MAX_DATASET_POOL_RAM_USAGE", nullptr);
        if (pszMaxRAMUsage)
        {
            l_nMaxRAMUsage = std::strtoll(pszMaxRAMUsage, nullptr, 10);
            if (strstr(pszMaxRAMUsage, "MB"))
                l_nMaxRAMUsage *= 1024 * 1024;
            else if (strstr(pszMaxRAMUsage, "GB"))
                l_nMaxRAMUsage *= 1024 * 1024 * 1024;
        }

        singleton =
            new GDALDatasetPool(GDALGetMaxDatasetPoolSize(), l_nMaxRAMUsage);
    }
    if (refCountOfDisabledRefCount == 0)
        singleton->refCount++;
}

/* keep that in sync with gdaldrivermanager.cpp */
void GDALDatasetPool::PreventDestroy()
{
    CPLMutexHolderD(GDALGetphDLMutex());
    if (!singleton)
        return;
    refCountOfDisabledRefCount++;
}

/* keep that in sync with gdaldrivermanager.cpp */
extern void GDALDatasetPoolPreventDestroy();

void GDALDatasetPoolPreventDestroy()
{
    GDALDatasetPool::PreventDestroy();
}

/************************************************************************/
/*                               Unref()                                */
/************************************************************************/

void GDALDatasetPool::Unref()
{
    CPLMutexHolderD(GDALGetphDLMutex());
    if (!singleton)
    {
        CPLAssert(false);
        return;
    }
    if (refCountOfDisabledRefCount == 0)
    {
        singleton->refCount--;
        if (singleton->refCount == 0)
        {
            delete singleton;
            singleton = nullptr;
        }
    }
}

/* keep that in sync with gdaldrivermanager.cpp */
void GDALDatasetPool::ForceDestroy()
{
    CPLMutexHolderD(GDALGetphDLMutex());
    if (!singleton)
        return;
    refCountOfDisabledRefCount--;
    CPLAssert(refCountOfDisabledRefCount == 0);
    singleton->refCount = 0;
    delete singleton;
    singleton = nullptr;
}

/* keep that in sync with gdaldrivermanager.cpp */
extern void GDALDatasetPoolForceDestroy();

void GDALDatasetPoolForceDestroy()
{
    GDALDatasetPool::ForceDestroy();
}

/************************************************************************/
/*                           RefDataset()                               */
/************************************************************************/

GDALProxyPoolCacheEntry *
GDALDatasetPool::RefDataset(const char *pszFileName, GDALAccess eAccess,
                            char **papszOpenOptions, int bShared,
                            bool bForceOpen, const char *pszOwner)
{
    return singleton->_RefDataset(pszFileName, eAccess, papszOpenOptions,
                                  bShared, bForceOpen, pszOwner);
}

/************************************************************************/
/*                       UnrefDataset()                                 */
/************************************************************************/

void GDALDatasetPool::UnrefDataset(GDALProxyPoolCacheEntry *cacheEntry)
{
    CPLMutexHolderD(GDALGetphDLMutex());
    cacheEntry->refCount--;
}

/************************************************************************/
/*                   CloseDatasetIfZeroRefCount()                       */
/************************************************************************/

void GDALDatasetPool::CloseDatasetIfZeroRefCount(const char *pszFileName,
                                                 CSLConstList papszOpenOptions,
                                                 GDALAccess eAccess,
                                                 const char *pszOwner)
{
    CPLMutexHolderD(GDALGetphDLMutex());
    singleton->_CloseDatasetIfZeroRefCount(pszFileName, papszOpenOptions,
                                           eAccess, pszOwner);
}

struct GetMetadataElt
{
    char *pszDomain;
    char **papszMetadata;
};

static unsigned long hash_func_get_metadata(const void *_elt)
{
    const GetMetadataElt *elt = static_cast<const GetMetadataElt *>(_elt);
    return CPLHashSetHashStr(elt->pszDomain);
}

static int equal_func_get_metadata(const void *_elt1, const void *_elt2)
{
    const GetMetadataElt *elt1 = static_cast<const GetMetadataElt *>(_elt1);
    const GetMetadataElt *elt2 = static_cast<const GetMetadataElt *>(_elt2);
    return CPLHashSetEqualStr(elt1->pszDomain, elt2->pszDomain);
}

static void free_func_get_metadata(void *_elt)
{
    GetMetadataElt *elt = static_cast<GetMetadataElt *>(_elt);
    CPLFree(elt->pszDomain);
    CSLDestroy(elt->papszMetadata);
    CPLFree(elt);
}

struct GetMetadataItemElt
{
    char *pszName;
    char *pszDomain;
    char *pszMetadataItem;
};

static unsigned long hash_func_get_metadata_item(const void *_elt)
{
    const GetMetadataItemElt *elt =
        static_cast<const GetMetadataItemElt *>(_elt);
    return CPLHashSetHashStr(elt->pszName) ^ CPLHashSetHashStr(elt->pszDomain);
}

static int equal_func_get_metadata_item(const void *_elt1, const void *_elt2)
{
    const GetMetadataItemElt *elt1 =
        static_cast<const GetMetadataItemElt *>(_elt1);
    const GetMetadataItemElt *elt2 =
        static_cast<const GetMetadataItemElt *>(_elt2);
    return CPLHashSetEqualStr(elt1->pszName, elt2->pszName) &&
           CPLHashSetEqualStr(elt1->pszDomain, elt2->pszDomain);
}

static void free_func_get_metadata_item(void *_elt)
{
    GetMetadataItemElt *elt = static_cast<GetMetadataItemElt *>(_elt);
    CPLFree(elt->pszName);
    CPLFree(elt->pszDomain);
    CPLFree(elt->pszMetadataItem);
    CPLFree(elt);
}

/* ******************************************************************** */
/*                     GDALProxyPoolDataset                             */
/* ******************************************************************** */

/* Note : the bShared parameter must be used with caution. You can */
/* set it to TRUE  for being used as a VRT source : in that case, */
/* VRTSimpleSource will take care of destroying it when there are no */
/* reference to it (in VRTSimpleSource::~VRTSimpleSource()) */
/* However this will not be registered as a genuine shared dataset, like it */
/* would have been with MarkAsShared(). But MarkAsShared() is not usable for */
/* GDALProxyPoolDataset objects, as they share the same description as their */
/* underlying dataset. So *NEVER* call MarkAsShared() on a GDALProxyPoolDataset
 */
/* object */

/* pszOwner is only honoured in the bShared case, and restrict the scope */
/* of the sharing. Only calls to _RefDataset() with the same value of */
/* pszOwner can effectively use the same dataset. The use case is */
/* to avoid 2 VRTs (potentially the same one) opened by a single thread,
 * pointing to */
/* the same source datasets. In that case, they would use the same dataset */
/* So even if the VRT handles themselves are used from different threads, since
 */
/* the underlying sources are shared, that might cause crashes (#6939). */
/* But we want to allow a same VRT referencing the same source dataset,*/
/* for example if it has multiple bands. So in practice the value of pszOwner */
/* is the serialized value (%p formatting) of the VRT dataset handle. */

GDALProxyPoolDataset::GDALProxyPoolDataset(
    const char *pszSourceDatasetDescription, int nRasterXSizeIn,
    int nRasterYSizeIn, GDALAccess eAccessIn, int bSharedIn,
    const char *pszProjectionRefIn, const GDALGeoTransform *pGT,
    const char *pszOwner)
    : responsiblePID(GDALGetResponsiblePIDForCurrentThread()),
      pszProjectionRef(pszProjectionRefIn ? CPLStrdup(pszProjectionRefIn)
                                          : nullptr)
{
    GDALDatasetPool::Ref();

    SetDescription(pszSourceDatasetDescription);

    nRasterXSize = nRasterXSizeIn;
    nRasterYSize = nRasterYSizeIn;
    eAccess = eAccessIn;

    bShared = CPL_TO_BOOL(bSharedIn);
    m_pszOwner = pszOwner ? CPLStrdup(pszOwner) : nullptr;

    if (pGT)
    {
        m_gt = *pGT;
        m_bHasSrcGeoTransform = true;
    }

    if (pszProjectionRefIn)
    {
        m_poSRS = new OGRSpatialReference();
        m_poSRS->importFromWkt(pszProjectionRefIn);
        m_bHasSrcSRS = true;
    }
}

/* Constructor where the parameters (raster size, etc.) are obtained
 * by opening the underlying dataset.
 */
GDALProxyPoolDataset::GDALProxyPoolDataset(
    const char *pszSourceDatasetDescription, GDALAccess eAccessIn,
    int bSharedIn, const char *pszOwner)
    : responsiblePID(GDALGetResponsiblePIDForCurrentThread())
{
    GDALDatasetPool::Ref();

    SetDescription(pszSourceDatasetDescription);

    eAccess = eAccessIn;

    bShared = CPL_TO_BOOL(bSharedIn);
    m_pszOwner = pszOwner ? CPLStrdup(pszOwner) : nullptr;
}

/************************************************************************/
/*                              Create()                                */
/************************************************************************/

/* Instantiate a GDALProxyPoolDataset where the parameters (raster size, etc.)
 * are obtained by opening the underlying dataset.
 * Its bands are also instantiated.
 */
GDALProxyPoolDataset *GDALProxyPoolDataset::Create(
    const char *pszSourceDatasetDescription, CSLConstList papszOpenOptionsIn,
    GDALAccess eAccessIn, int bSharedIn, const char *pszOwner)
{
    std::unique_ptr<GDALProxyPoolDataset> poSelf(new GDALProxyPoolDataset(
        pszSourceDatasetDescription, eAccessIn, bSharedIn, pszOwner));
    poSelf->SetOpenOptions(papszOpenOptionsIn);
    GDALDataset *poUnderlyingDS = poSelf->RefUnderlyingDataset();
    if (!poUnderlyingDS)
        return nullptr;
    poSelf->nRasterXSize = poUnderlyingDS->GetRasterXSize();
    poSelf->nRasterYSize = poUnderlyingDS->GetRasterYSize();
    if (poUnderlyingDS->GetGeoTransform(poSelf->m_gt) == CE_None)
        poSelf->m_bHasSrcGeoTransform = true;
    const auto poSRS = poUnderlyingDS->GetSpatialRef();
    if (poSRS)
    {
        poSelf->m_poSRS = poSRS->Clone();
        poSelf->m_bHasSrcSRS = true;
    }
    for (int i = 1; i <= poUnderlyingDS->GetRasterCount(); ++i)
    {
        auto poSrcBand = poUnderlyingDS->GetRasterBand(i);
        if (!poSrcBand)
        {
            poSelf->UnrefUnderlyingDataset(poUnderlyingDS);
            return nullptr;
        }
        int nSrcBlockXSize, nSrcBlockYSize;
        poSrcBand->GetBlockSize(&nSrcBlockXSize, &nSrcBlockYSize);
        poSelf->AddSrcBandDescription(poSrcBand->GetRasterDataType(),
                                      nSrcBlockXSize, nSrcBlockYSize);
    }
    poSelf->UnrefUnderlyingDataset(poUnderlyingDS);
    return poSelf.release();
}

/************************************************************************/
/*                    ~GDALProxyPoolDataset()                           */
/************************************************************************/

GDALProxyPoolDataset::~GDALProxyPoolDataset()
{
    GDALDatasetPool::CloseDatasetIfZeroRefCount(
        GetDescription(), papszOpenOptions, eAccess, m_pszOwner);

    /* See comment in constructor */
    /* It is not really a genuine shared dataset, so we don't */
    /* want ~GDALDataset() to try to release it from its */
    /* shared dataset hashset. This will save a */
    /* "Should not happen. Cannot find %s, this=%p in phSharedDatasetSet" debug
     * message */
    bShared = false;

    CPLFree(pszProjectionRef);
    CPLFree(pszGCPProjection);
    if (nGCPCount)
    {
        GDALDeinitGCPs(nGCPCount, pasGCPList);
        CPLFree(pasGCPList);
    }
    if (metadataSet)
        CPLHashSetDestroy(metadataSet);
    if (metadataItemSet)
        CPLHashSetDestroy(metadataItemSet);
    CPLFree(m_pszOwner);
    if (m_poSRS)
        m_poSRS->Release();
    if (m_poGCPSRS)
        m_poGCPSRS->Release();

    GDALDatasetPool::Unref();
}

/************************************************************************/
/*                        SetOpenOptions()                              */
/************************************************************************/

void GDALProxyPoolDataset::SetOpenOptions(CSLConstList papszOpenOptionsIn)
{
    CPLAssert(papszOpenOptions == nullptr);
    papszOpenOptions = CSLDuplicate(papszOpenOptionsIn);
}

/************************************************************************/
/*                    AddSrcBandDescription()                           */
/************************************************************************/

void GDALProxyPoolDataset::AddSrcBandDescription(GDALDataType eDataType,
                                                 int nBlockXSize,
                                                 int nBlockYSize)
{
    SetBand(nBands + 1, new GDALProxyPoolRasterBand(this, nBands + 1, eDataType,
                                                    nBlockXSize, nBlockYSize));
}

/************************************************************************/
/*                    AddSrcBand()                                      */
/************************************************************************/

void GDALProxyPoolDataset::AddSrcBand(int nBand, GDALDataType eDataType,
                                      int nBlockXSize, int nBlockYSize)
{
    SetBand(nBand, new GDALProxyPoolRasterBand(this, nBand, eDataType,
                                               nBlockXSize, nBlockYSize));
}

/************************************************************************/
/*                    RefUnderlyingDataset()                            */
/************************************************************************/

GDALDataset *GDALProxyPoolDataset::RefUnderlyingDataset() const
{
    return RefUnderlyingDataset(true);
}

GDALDataset *GDALProxyPoolDataset::RefUnderlyingDataset(bool bForceOpen) const
{
    /* We pretend that the current thread is responsiblePID, that is */
    /* to say the thread that created that GDALProxyPoolDataset object. */
    /* This is for the case when a GDALProxyPoolDataset is created by a */
    /* thread and used by other threads. These other threads, when doing actual
     */
    /* IO, will come there and potentially open the underlying dataset. */
    /* By doing this, they can indirectly call GDALOpenShared() on .aux file */
    /* for example. So this call to GDALOpenShared() must occur as if it */
    /* was done by the creating thread, otherwise it will not be correctly
     * closed afterwards... */
    /* To make a long story short : this is necessary when warping with
     * ChunkAndWarpMulti */
    /* a VRT of GeoTIFFs that have associated .aux files */
    GIntBig curResponsiblePID = GDALGetResponsiblePIDForCurrentThread();
    GDALSetResponsiblePIDForCurrentThread(responsiblePID);
    cacheEntry =
        GDALDatasetPool::RefDataset(GetDescription(), eAccess, papszOpenOptions,
                                    GetShared(), bForceOpen, m_pszOwner);
    GDALSetResponsiblePIDForCurrentThread(curResponsiblePID);
    if (cacheEntry != nullptr)
    {
        if (cacheEntry->poDS != nullptr)
            return cacheEntry->poDS;
        else
            GDALDatasetPool::UnrefDataset(cacheEntry);
    }
    return nullptr;
}

/************************************************************************/
/*                    UnrefUnderlyingDataset()                        */
/************************************************************************/

void GDALProxyPoolDataset::UnrefUnderlyingDataset(
    CPL_UNUSED GDALDataset *poUnderlyingDataset) const
{
    if (cacheEntry != nullptr)
    {
        CPLAssert(cacheEntry->poDS == poUnderlyingDataset);
        if (cacheEntry->poDS != nullptr)
            GDALDatasetPool::UnrefDataset(cacheEntry);
    }
}

/************************************************************************/
/*                         FlushCache()                                 */
/************************************************************************/

CPLErr GDALProxyPoolDataset::FlushCache(bool bAtClosing)
{
    CPLErr eErr = CE_None;
    GDALDataset *poUnderlyingDataset = RefUnderlyingDataset(false);
    if (poUnderlyingDataset)
    {
        eErr = poUnderlyingDataset->FlushCache(bAtClosing);
        UnrefUnderlyingDataset(poUnderlyingDataset);
    }
    return eErr;
}

/************************************************************************/
/*                        SetSpatialRef()                               */
/************************************************************************/

CPLErr GDALProxyPoolDataset::SetSpatialRef(const OGRSpatialReference *poSRS)
{
    m_bHasSrcSRS = false;
    return GDALProxyDataset::SetSpatialRef(poSRS);
}

/************************************************************************/
/*                        GetSpatialRef()                               */
/************************************************************************/

const OGRSpatialReference *GDALProxyPoolDataset::GetSpatialRef() const
{
    if (m_bHasSrcSRS)
        return m_poSRS;
    else
    {
        if (m_poSRS)
            m_poSRS->Release();
        m_poSRS = nullptr;
        auto poSRS = GDALProxyDataset::GetSpatialRef();
        if (poSRS)
            m_poSRS = poSRS->Clone();
        return m_poSRS;
    }
}

/************************************************************************/
/*                        SetGeoTransform()                             */
/************************************************************************/

CPLErr GDALProxyPoolDataset::SetGeoTransform(const GDALGeoTransform &gt)
{
    m_gt = gt;
    m_bHasSrcGeoTransform = false;
    return GDALProxyDataset::SetGeoTransform(gt);
}

/************************************************************************/
/*                        GetGeoTransform()                             */
/************************************************************************/

CPLErr GDALProxyPoolDataset::GetGeoTransform(GDALGeoTransform &gt) const
{
    if (m_bHasSrcGeoTransform)
    {
        gt = m_gt;
        return CE_None;
    }
    else
    {
        return GDALProxyDataset::GetGeoTransform(gt);
    }
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **GDALProxyPoolDataset::GetMetadata(const char *pszDomain)
{
    if (metadataSet == nullptr)
        metadataSet =
            CPLHashSetNew(hash_func_get_metadata, equal_func_get_metadata,
                          free_func_get_metadata);

    GDALDataset *poUnderlyingDataset = RefUnderlyingDataset();
    if (poUnderlyingDataset == nullptr)
        return nullptr;

    char **papszUnderlyingMetadata =
        poUnderlyingDataset->GetMetadata(pszDomain);

    GetMetadataElt *pElt =
        static_cast<GetMetadataElt *>(CPLMalloc(sizeof(GetMetadataElt)));
    pElt->pszDomain = (pszDomain) ? CPLStrdup(pszDomain) : nullptr;
    pElt->papszMetadata = CSLDuplicate(papszUnderlyingMetadata);
    CPLHashSetInsert(metadataSet, pElt);

    UnrefUnderlyingDataset(poUnderlyingDataset);

    return pElt->papszMetadata;
}

/************************************************************************/
/*                        GetMetadataItem()                             */
/************************************************************************/

const char *GDALProxyPoolDataset::GetMetadataItem(const char *pszName,
                                                  const char *pszDomain)
{
    if (metadataItemSet == nullptr)
        metadataItemSet = CPLHashSetNew(hash_func_get_metadata_item,
                                        equal_func_get_metadata_item,
                                        free_func_get_metadata_item);

    GDALDataset *poUnderlyingDataset = RefUnderlyingDataset();
    if (poUnderlyingDataset == nullptr)
        return nullptr;

    const char *pszUnderlyingMetadataItem =
        poUnderlyingDataset->GetMetadataItem(pszName, pszDomain);

    GetMetadataItemElt *pElt = static_cast<GetMetadataItemElt *>(
        CPLMalloc(sizeof(GetMetadataItemElt)));
    pElt->pszName = (pszName) ? CPLStrdup(pszName) : nullptr;
    pElt->pszDomain = (pszDomain) ? CPLStrdup(pszDomain) : nullptr;
    pElt->pszMetadataItem = (pszUnderlyingMetadataItem)
                                ? CPLStrdup(pszUnderlyingMetadataItem)
                                : nullptr;
    CPLHashSetInsert(metadataItemSet, pElt);

    UnrefUnderlyingDataset(poUnderlyingDataset);

    return pElt->pszMetadataItem;
}

/************************************************************************/
/*                      GetInternalHandle()                             */
/************************************************************************/

void *GDALProxyPoolDataset::GetInternalHandle(const char *pszRequest)
{
    CPLError(
        CE_Warning, CPLE_AppDefined,
        "GetInternalHandle() cannot be safely called on a proxy pool dataset\n"
        "as the returned value may be invalidated at any time.\n");
    return GDALProxyDataset::GetInternalHandle(pszRequest);
}

/************************************************************************/
/*                     GetGCPSpatialRef()                               */
/************************************************************************/

const OGRSpatialReference *GDALProxyPoolDataset::GetGCPSpatialRef() const
{
    GDALDataset *poUnderlyingDataset = RefUnderlyingDataset();
    if (poUnderlyingDataset == nullptr)
        return nullptr;

    m_poGCPSRS->Release();
    m_poGCPSRS = nullptr;

    const auto poUnderlyingGCPSRS = poUnderlyingDataset->GetGCPSpatialRef();
    if (poUnderlyingGCPSRS)
        m_poGCPSRS = poUnderlyingGCPSRS->Clone();

    UnrefUnderlyingDataset(poUnderlyingDataset);

    return m_poGCPSRS;
}

/************************************************************************/
/*                            GetGCPs()                                 */
/************************************************************************/

const GDAL_GCP *GDALProxyPoolDataset::GetGCPs()
{
    GDALDataset *poUnderlyingDataset = RefUnderlyingDataset();
    if (poUnderlyingDataset == nullptr)
        return nullptr;

    if (nGCPCount)
    {
        GDALDeinitGCPs(nGCPCount, pasGCPList);
        CPLFree(pasGCPList);
        pasGCPList = nullptr;
    }

    const GDAL_GCP *pasUnderlyingGCPList = poUnderlyingDataset->GetGCPs();
    nGCPCount = poUnderlyingDataset->GetGCPCount();
    if (nGCPCount)
        pasGCPList = GDALDuplicateGCPs(nGCPCount, pasUnderlyingGCPList);

    UnrefUnderlyingDataset(poUnderlyingDataset);

    return pasGCPList;
}

/************************************************************************/
/*                     GDALProxyPoolDatasetCreate()                     */
/************************************************************************/

GDALProxyPoolDatasetH GDALProxyPoolDatasetCreate(
    const char *pszSourceDatasetDescription, int nRasterXSize, int nRasterYSize,
    GDALAccess eAccess, int bShared, const char *pszProjectionRef,
    const double *padfGeoTransform)
{
    return reinterpret_cast<GDALProxyPoolDatasetH>(new GDALProxyPoolDataset(
        pszSourceDatasetDescription, nRasterXSize, nRasterYSize, eAccess,
        bShared, pszProjectionRef,
        reinterpret_cast<const GDALGeoTransform *>(padfGeoTransform)));
}

/************************************************************************/
/*                       GDALProxyPoolDatasetDelete()                   */
/************************************************************************/

void GDALProxyPoolDatasetDelete(GDALProxyPoolDatasetH hProxyPoolDataset)
{
    delete reinterpret_cast<GDALProxyPoolDataset *>(hProxyPoolDataset);
}

/************************************************************************/
/*              GDALProxyPoolDatasetAddSrcBandDescription()             */
/************************************************************************/

void GDALProxyPoolDatasetAddSrcBandDescription(
    GDALProxyPoolDatasetH hProxyPoolDataset, GDALDataType eDataType,
    int nBlockXSize, int nBlockYSize)
{
    reinterpret_cast<GDALProxyPoolDataset *>(hProxyPoolDataset)
        ->AddSrcBandDescription(eDataType, nBlockXSize, nBlockYSize);
}

/* ******************************************************************** */
/*                    GDALProxyPoolRasterBand()                         */
/* ******************************************************************** */

GDALProxyPoolRasterBand::GDALProxyPoolRasterBand(GDALProxyPoolDataset *poDSIn,
                                                 int nBandIn,
                                                 GDALDataType eDataTypeIn,
                                                 int nBlockXSizeIn,
                                                 int nBlockYSizeIn)
{
    poDS = poDSIn;
    nBand = nBandIn;
    eDataType = eDataTypeIn;
    nRasterXSize = poDSIn->GetRasterXSize();
    nRasterYSize = poDSIn->GetRasterYSize();
    nBlockXSize = nBlockXSizeIn;
    nBlockYSize = nBlockYSizeIn;
}

/* ******************************************************************** */
/*                    GDALProxyPoolRasterBand()                         */
/* ******************************************************************** */

GDALProxyPoolRasterBand::GDALProxyPoolRasterBand(
    GDALProxyPoolDataset *poDSIn, GDALRasterBand *poUnderlyingRasterBand)
{
    poDS = poDSIn;
    nBand = poUnderlyingRasterBand->GetBand();
    eDataType = poUnderlyingRasterBand->GetRasterDataType();
    nRasterXSize = poUnderlyingRasterBand->GetXSize();
    nRasterYSize = poUnderlyingRasterBand->GetYSize();
    poUnderlyingRasterBand->GetBlockSize(&nBlockXSize, &nBlockYSize);
}

/* ******************************************************************** */
/*                   ~GDALProxyPoolRasterBand()                         */
/* ******************************************************************** */
GDALProxyPoolRasterBand::~GDALProxyPoolRasterBand()
{
    if (metadataSet)
        CPLHashSetDestroy(metadataSet);
    if (metadataItemSet)
        CPLHashSetDestroy(metadataItemSet);
    CPLFree(pszUnitType);
    CSLDestroy(papszCategoryNames);
    if (poColorTable)
        delete poColorTable;

    for (int i = 0; i < nSizeProxyOverviewRasterBand; i++)
    {
        if (papoProxyOverviewRasterBand[i])
            delete papoProxyOverviewRasterBand[i];
    }
    CPLFree(papoProxyOverviewRasterBand);
    if (poProxyMaskBand)
        delete poProxyMaskBand;
}

/************************************************************************/
/*                AddSrcMaskBandDescriptionFromUnderlying()             */
/************************************************************************/

void GDALProxyPoolRasterBand::AddSrcMaskBandDescriptionFromUnderlying()
{
    if (poProxyMaskBand != nullptr)
        return;
    GDALRasterBand *poUnderlyingBand = RefUnderlyingRasterBand();
    if (poUnderlyingBand == nullptr)
        return;
    auto poSrcMaskBand = poUnderlyingBand->GetMaskBand();
    int nSrcBlockXSize, nSrcBlockYSize;
    poSrcMaskBand->GetBlockSize(&nSrcBlockXSize, &nSrcBlockYSize);
    poProxyMaskBand = new GDALProxyPoolMaskBand(
        cpl::down_cast<GDALProxyPoolDataset *>(poDS), this,
        poSrcMaskBand->GetRasterDataType(), nSrcBlockXSize, nSrcBlockYSize);
    UnrefUnderlyingRasterBand(poUnderlyingBand);
}

/************************************************************************/
/*                 AddSrcMaskBandDescription()                          */
/************************************************************************/

void GDALProxyPoolRasterBand::AddSrcMaskBandDescription(
    GDALDataType eDataTypeIn, int nBlockXSizeIn, int nBlockYSizeIn)
{
    CPLAssert(poProxyMaskBand == nullptr);
    poProxyMaskBand = new GDALProxyPoolMaskBand(
        cpl::down_cast<GDALProxyPoolDataset *>(poDS), this, eDataTypeIn,
        nBlockXSizeIn, nBlockYSizeIn);
}

/************************************************************************/
/*                  RefUnderlyingRasterBand()                           */
/************************************************************************/

GDALRasterBand *
GDALProxyPoolRasterBand::RefUnderlyingRasterBand(bool bForceOpen) const
{
    GDALDataset *poUnderlyingDataset =
        (cpl::down_cast<GDALProxyPoolDataset *>(poDS))
            ->RefUnderlyingDataset(bForceOpen);
    if (poUnderlyingDataset == nullptr)
        return nullptr;

    GDALRasterBand *poBand = poUnderlyingDataset->GetRasterBand(nBand);
    if (poBand == nullptr)
    {
        (cpl::down_cast<GDALProxyPoolDataset *>(poDS))
            ->UnrefUnderlyingDataset(poUnderlyingDataset);
    }
    else if (nBlockXSize <= 0 || nBlockYSize <= 0)
    {
        // Here we try to load nBlockXSize&nBlockYSize from underlying band
        // but we must guarantee that we will not access directly to
        // nBlockXSize/nBlockYSize before RefUnderlyingRasterBand() is called
        int nSrcBlockXSize, nSrcBlockYSize;
        poBand->GetBlockSize(&nSrcBlockXSize, &nSrcBlockYSize);
        const_cast<GDALProxyPoolRasterBand *>(this)->nBlockXSize =
            nSrcBlockXSize;
        const_cast<GDALProxyPoolRasterBand *>(this)->nBlockYSize =
            nSrcBlockYSize;
    }

    return poBand;
}

/************************************************************************/
/*                  UnrefUnderlyingRasterBand()                       */
/************************************************************************/

void GDALProxyPoolRasterBand::UnrefUnderlyingRasterBand(
    GDALRasterBand *poUnderlyingRasterBand) const
{
    if (poUnderlyingRasterBand)
        (cpl::down_cast<GDALProxyPoolDataset *>(poDS))
            ->UnrefUnderlyingDataset(poUnderlyingRasterBand->GetDataset());
}

/************************************************************************/
/*                             FlushCache()                             */
/************************************************************************/

CPLErr GDALProxyPoolRasterBand::FlushCache(bool bAtClosing)
{
    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand(false);
    if (poUnderlyingRasterBand)
    {
        CPLErr eErr = poUnderlyingRasterBand->FlushCache(bAtClosing);
        UnrefUnderlyingRasterBand(poUnderlyingRasterBand);
        return eErr;
    }
    return CE_None;
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **GDALProxyPoolRasterBand::GetMetadata(const char *pszDomain)
{
    if (metadataSet == nullptr)
        metadataSet =
            CPLHashSetNew(hash_func_get_metadata, equal_func_get_metadata,
                          free_func_get_metadata);

    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand();
    if (poUnderlyingRasterBand == nullptr)
        return nullptr;

    char **papszUnderlyingMetadata =
        poUnderlyingRasterBand->GetMetadata(pszDomain);

    GetMetadataElt *pElt =
        static_cast<GetMetadataElt *>(CPLMalloc(sizeof(GetMetadataElt)));
    pElt->pszDomain = (pszDomain) ? CPLStrdup(pszDomain) : nullptr;
    pElt->papszMetadata = CSLDuplicate(papszUnderlyingMetadata);
    CPLHashSetInsert(metadataSet, pElt);

    UnrefUnderlyingRasterBand(poUnderlyingRasterBand);

    return pElt->papszMetadata;
}

/************************************************************************/
/*                        GetMetadataItem()                             */
/************************************************************************/

const char *GDALProxyPoolRasterBand::GetMetadataItem(const char *pszName,
                                                     const char *pszDomain)
{
    if (metadataItemSet == nullptr)
        metadataItemSet = CPLHashSetNew(hash_func_get_metadata_item,
                                        equal_func_get_metadata_item,
                                        free_func_get_metadata_item);

    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand();
    if (poUnderlyingRasterBand == nullptr)
        return nullptr;

    const char *pszUnderlyingMetadataItem =
        poUnderlyingRasterBand->GetMetadataItem(pszName, pszDomain);

    GetMetadataItemElt *pElt = static_cast<GetMetadataItemElt *>(
        CPLMalloc(sizeof(GetMetadataItemElt)));
    pElt->pszName = (pszName) ? CPLStrdup(pszName) : nullptr;
    pElt->pszDomain = (pszDomain) ? CPLStrdup(pszDomain) : nullptr;
    pElt->pszMetadataItem = (pszUnderlyingMetadataItem)
                                ? CPLStrdup(pszUnderlyingMetadataItem)
                                : nullptr;
    CPLHashSetInsert(metadataItemSet, pElt);

    UnrefUnderlyingRasterBand(poUnderlyingRasterBand);

    return pElt->pszMetadataItem;
}

/* ******************************************************************** */
/*                       GetCategoryNames()                             */
/* ******************************************************************** */

char **GDALProxyPoolRasterBand::GetCategoryNames()
{
    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand();
    if (poUnderlyingRasterBand == nullptr)
        return nullptr;

    CSLDestroy(papszCategoryNames);
    papszCategoryNames = nullptr;

    char **papszUnderlyingCategoryNames =
        poUnderlyingRasterBand->GetCategoryNames();
    if (papszUnderlyingCategoryNames)
        papszCategoryNames = CSLDuplicate(papszUnderlyingCategoryNames);

    UnrefUnderlyingRasterBand(poUnderlyingRasterBand);

    return papszCategoryNames;
}

/* ******************************************************************** */
/*                           GetUnitType()                              */
/* ******************************************************************** */

const char *GDALProxyPoolRasterBand::GetUnitType()
{
    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand();
    if (poUnderlyingRasterBand == nullptr)
        return nullptr;

    CPLFree(pszUnitType);
    pszUnitType = nullptr;

    const char *pszUnderlyingUnitType = poUnderlyingRasterBand->GetUnitType();
    if (pszUnderlyingUnitType)
        pszUnitType = CPLStrdup(pszUnderlyingUnitType);

    UnrefUnderlyingRasterBand(poUnderlyingRasterBand);

    return pszUnitType;
}

/* ******************************************************************** */
/*                          GetColorTable()                             */
/* ******************************************************************** */

GDALColorTable *GDALProxyPoolRasterBand::GetColorTable()
{
    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand();
    if (poUnderlyingRasterBand == nullptr)
        return nullptr;

    if (poColorTable)
        delete poColorTable;
    poColorTable = nullptr;

    GDALColorTable *poUnderlyingColorTable =
        poUnderlyingRasterBand->GetColorTable();
    if (poUnderlyingColorTable)
        poColorTable = poUnderlyingColorTable->Clone();

    UnrefUnderlyingRasterBand(poUnderlyingRasterBand);

    return poColorTable;
}

/* ******************************************************************** */
/*                           GetOverview()                              */
/* ******************************************************************** */

GDALRasterBand *GDALProxyPoolRasterBand::GetOverview(int nOverviewBand)
{
    if (nOverviewBand >= 0 && nOverviewBand < nSizeProxyOverviewRasterBand)
    {
        if (papoProxyOverviewRasterBand[nOverviewBand])
            return papoProxyOverviewRasterBand[nOverviewBand];
    }

    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand();
    if (poUnderlyingRasterBand == nullptr)
        return nullptr;

    GDALRasterBand *poOverviewRasterBand =
        poUnderlyingRasterBand->GetOverview(nOverviewBand);
    if (poOverviewRasterBand == nullptr)
    {
        UnrefUnderlyingRasterBand(poUnderlyingRasterBand);
        return nullptr;
    }

    if (nOverviewBand >= nSizeProxyOverviewRasterBand)
    {
        papoProxyOverviewRasterBand =
            static_cast<GDALProxyPoolOverviewRasterBand **>(
                CPLRealloc(papoProxyOverviewRasterBand,
                           sizeof(GDALProxyPoolOverviewRasterBand *) *
                               (nOverviewBand + 1)));
        for (int i = nSizeProxyOverviewRasterBand; i < nOverviewBand + 1; i++)
            papoProxyOverviewRasterBand[i] = nullptr;
        nSizeProxyOverviewRasterBand = nOverviewBand + 1;
    }

    papoProxyOverviewRasterBand[nOverviewBand] =
        new GDALProxyPoolOverviewRasterBand(
            cpl::down_cast<GDALProxyPoolDataset *>(poDS), poOverviewRasterBand,
            this, nOverviewBand);

    UnrefUnderlyingRasterBand(poUnderlyingRasterBand);

    return papoProxyOverviewRasterBand[nOverviewBand];
}

/* ******************************************************************** */
/*                     GetRasterSampleOverview()                        */
/* ******************************************************************** */

GDALRasterBand *
GDALProxyPoolRasterBand::GetRasterSampleOverview(GUIntBig /* nDesiredSamples */)
{
    CPLError(CE_Failure, CPLE_AppDefined,
             "GDALProxyPoolRasterBand::GetRasterSampleOverview : not "
             "implemented yet");
    return nullptr;
}

/* ******************************************************************** */
/*                           GetMaskBand()                              */
/* ******************************************************************** */

GDALRasterBand *GDALProxyPoolRasterBand::GetMaskBand()
{
    if (poProxyMaskBand)
        return poProxyMaskBand;

    GDALRasterBand *poUnderlyingRasterBand = RefUnderlyingRasterBand();
    if (poUnderlyingRasterBand == nullptr)
        return nullptr;

    GDALRasterBand *poMaskBand = poUnderlyingRasterBand->GetMaskBand();

    poProxyMaskBand = new GDALProxyPoolMaskBand(
        cpl::down_cast<GDALProxyPoolDataset *>(poDS), poMaskBand, this);

    UnrefUnderlyingRasterBand(poUnderlyingRasterBand);

    return poProxyMaskBand;
}

/* ******************************************************************** */
/*             GDALProxyPoolOverviewRasterBand()                        */
/* ******************************************************************** */

GDALProxyPoolOverviewRasterBand::GDALProxyPoolOverviewRasterBand(
    GDALProxyPoolDataset *poDSIn, GDALRasterBand *poUnderlyingOverviewBand,
    GDALProxyPoolRasterBand *poMainBandIn, int nOverviewBandIn)
    : GDALProxyPoolRasterBand(poDSIn, poUnderlyingOverviewBand),
      poMainBand(poMainBandIn), nOverviewBand(nOverviewBandIn)
{
}

/* ******************************************************************** */
/*                  ~GDALProxyPoolOverviewRasterBand()                  */
/* ******************************************************************** */

GDALProxyPoolOverviewRasterBand::~GDALProxyPoolOverviewRasterBand()
{
    CPLAssert(nRefCountUnderlyingMainRasterBand == 0);
}

/* ******************************************************************** */
/*                    RefUnderlyingRasterBand()                         */
/* ******************************************************************** */

GDALRasterBand *
GDALProxyPoolOverviewRasterBand::RefUnderlyingRasterBand(bool bForceOpen) const
{
    poUnderlyingMainRasterBand =
        poMainBand->RefUnderlyingRasterBand(bForceOpen);
    if (poUnderlyingMainRasterBand == nullptr)
        return nullptr;

    nRefCountUnderlyingMainRasterBand++;
    return poUnderlyingMainRasterBand->GetOverview(nOverviewBand);
}

/* ******************************************************************** */
/*                  UnrefUnderlyingRasterBand()                         */
/* ******************************************************************** */

void GDALProxyPoolOverviewRasterBand::UnrefUnderlyingRasterBand(
    GDALRasterBand * /* poUnderlyingRasterBand */) const
{
    poMainBand->UnrefUnderlyingRasterBand(poUnderlyingMainRasterBand);
    nRefCountUnderlyingMainRasterBand--;
}

/* ******************************************************************** */
/*                     GDALProxyPoolMaskBand()                          */
/* ******************************************************************** */

GDALProxyPoolMaskBand::GDALProxyPoolMaskBand(
    GDALProxyPoolDataset *poDSIn, GDALRasterBand *poUnderlyingMaskBand,
    GDALProxyPoolRasterBand *poMainBandIn)
    : GDALProxyPoolRasterBand(poDSIn, poUnderlyingMaskBand)
{
    poMainBand = poMainBandIn;

    poUnderlyingMainRasterBand = nullptr;
    nRefCountUnderlyingMainRasterBand = 0;
}

/* ******************************************************************** */
/*                     GDALProxyPoolMaskBand()                          */
/* ******************************************************************** */

GDALProxyPoolMaskBand::GDALProxyPoolMaskBand(
    GDALProxyPoolDataset *poDSIn, GDALProxyPoolRasterBand *poMainBandIn,
    GDALDataType eDataTypeIn, int nBlockXSizeIn, int nBlockYSizeIn)
    : GDALProxyPoolRasterBand(poDSIn, 1, eDataTypeIn, nBlockXSizeIn,
                              nBlockYSizeIn),
      poMainBand(poMainBandIn)
{
}

/* ******************************************************************** */
/*                          ~GDALProxyPoolMaskBand()                    */
/* ******************************************************************** */

GDALProxyPoolMaskBand::~GDALProxyPoolMaskBand()
{
    CPLAssert(nRefCountUnderlyingMainRasterBand == 0);
}

/* ******************************************************************** */
/*                    RefUnderlyingRasterBand()                         */
/* ******************************************************************** */

GDALRasterBand *
GDALProxyPoolMaskBand::RefUnderlyingRasterBand(bool bForceOpen) const
{
    poUnderlyingMainRasterBand =
        poMainBand->RefUnderlyingRasterBand(bForceOpen);
    if (poUnderlyingMainRasterBand == nullptr)
        return nullptr;

    nRefCountUnderlyingMainRasterBand++;
    return poUnderlyingMainRasterBand->GetMaskBand();
}

/* ******************************************************************** */
/*                  UnrefUnderlyingRasterBand()                         */
/* ******************************************************************** */

void GDALProxyPoolMaskBand::UnrefUnderlyingRasterBand(
    GDALRasterBand * /* poUnderlyingRasterBand */) const
{
    poMainBand->UnrefUnderlyingRasterBand(poUnderlyingMainRasterBand);
    nRefCountUnderlyingMainRasterBand--;
}

//! @endcond
