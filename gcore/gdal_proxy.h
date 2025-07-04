/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  GDAL Core C++/Private declarations
 * Author:   Even Rouault <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef GDAL_PROXY_H_INCLUDED
#define GDAL_PROXY_H_INCLUDED

#ifndef DOXYGEN_SKIP

#include "gdal.h"

#ifdef __cplusplus

#include "gdal_priv.h"
#include "cpl_hash_set.h"

/* ******************************************************************** */
/*                        GDALProxyDataset                              */
/* ******************************************************************** */

class CPL_DLL GDALProxyDataset : public GDALDataset
{
  protected:
    GDALProxyDataset()
    {
    }

    virtual GDALDataset *RefUnderlyingDataset() const = 0;
    virtual void UnrefUnderlyingDataset(GDALDataset *poUnderlyingDataset) const;

    CPLErr IBuildOverviews(const char *, int, const int *, int, const int *,
                           GDALProgressFunc, void *,
                           CSLConstList papszOptions) override;
    CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                     GDALDataType, int, BANDMAP_TYPE, GSpacing, GSpacing,
                     GSpacing, GDALRasterIOExtraArg *psExtraArg) override;
    CPLErr BlockBasedRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                              int nXSize, int nYSize, void *pData,
                              int nBufXSize, int nBufYSize,
                              GDALDataType eBufType, int nBandCount,
                              const int *panBandMap, GSpacing nPixelSpace,
                              GSpacing nLineSpace, GSpacing nBandSpace,
                              GDALRasterIOExtraArg *psExtraArg) override;

  public:
    char **GetMetadataDomainList() override;
    char **GetMetadata(const char *pszDomain) override;
    CPLErr SetMetadata(char **papszMetadata, const char *pszDomain) override;
    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain) override;
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain) override;

    CPLErr FlushCache(bool bAtClosing) override;

    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

    CPLErr GetGeoTransform(GDALGeoTransform &) const override;
    CPLErr SetGeoTransform(const GDALGeoTransform &) override;

    void *GetInternalHandle(const char *) override;
    GDALDriver *GetDriver() override;
    char **GetFileList() override;

    int GetGCPCount() override;
    const OGRSpatialReference *GetGCPSpatialRef() const override;
    const GDAL_GCP *GetGCPs() override;
    CPLErr SetGCPs(int nGCPCount, const GDAL_GCP *pasGCPList,
                   const OGRSpatialReference *poGCP_SRS) override;

    CPLErr AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
                      int nBufXSize, int nBufYSize, GDALDataType eDT,
                      int nBandCount, int *panBandList,
                      char **papszOptions) override;

    CPLErr CreateMaskBand(int nFlags) override;

    virtual CPLStringList
    GetCompressionFormats(int nXOff, int nYOff, int nXSize, int nYSize,
                          int nBandCount, const int *panBandList) override;
    virtual CPLErr ReadCompressedData(const char *pszFormat, int nXOff,
                                      int nYOff, int nXSize, int nYSize,
                                      int nBandCount, const int *panBandList,
                                      void **ppBuffer, size_t *pnBufferSize,
                                      char **ppszDetailedFormat) override;

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALProxyDataset)
};

/* ******************************************************************** */
/*                         GDALProxyRasterBand                          */
/* ******************************************************************** */

class CPL_DLL GDALProxyRasterBand : public GDALRasterBand
{
  protected:
    GDALProxyRasterBand()
    {
    }

    virtual GDALRasterBand *
    RefUnderlyingRasterBand(bool bForceOpen = true) const = 0;
    virtual void
    UnrefUnderlyingRasterBand(GDALRasterBand *poUnderlyingRasterBand) const;

    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IWriteBlock(int, int, void *) override;
    CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                     GDALDataType, GSpacing, GSpacing,
                     GDALRasterIOExtraArg *psExtraArg) override;

    int IGetDataCoverageStatus(int nXOff, int nYOff, int nXSize, int nYSize,
                               int nMaskFlagStop, double *pdfDataPct) override;

  public:
    char **GetMetadataDomainList() override;
    char **GetMetadata(const char *pszDomain) override;
    CPLErr SetMetadata(char **papszMetadata, const char *pszDomain) override;
    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain) override;
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain) override;

    GDALRasterBlock *GetLockedBlockRef(int nXBlockOff, int nYBlockOff,
                                       int bJustInitialize) override;

    GDALRasterBlock *TryGetLockedBlockRef(int nXBlockOff,
                                          int nYBlockYOff) override;

    CPLErr FlushBlock(int nXBlockOff, int nYBlockOff,
                      int bWriteDirtyBlock) override;

    CPLErr FlushCache(bool bAtClosing) override;
    char **GetCategoryNames() override;
    double GetNoDataValue(int *pbSuccess = nullptr) override;
    double GetMinimum(int *pbSuccess = nullptr) override;
    double GetMaximum(int *pbSuccess = nullptr) override;
    double GetOffset(int *pbSuccess = nullptr) override;
    double GetScale(int *pbSuccess = nullptr) override;
    const char *GetUnitType() override;
    GDALColorInterp GetColorInterpretation() override;
    GDALColorTable *GetColorTable() override;
    CPLErr Fill(double dfRealValue, double dfImaginaryValue = 0) override;

    CPLErr SetCategoryNames(char **) override;
    CPLErr SetNoDataValue(double) override;
    CPLErr DeleteNoDataValue() override;
    CPLErr SetColorTable(GDALColorTable *) override;
    CPLErr SetColorInterpretation(GDALColorInterp) override;
    CPLErr SetOffset(double) override;
    CPLErr SetScale(double) override;
    CPLErr SetUnitType(const char *) override;

    CPLErr GetStatistics(int bApproxOK, int bForce, double *pdfMin,
                         double *pdfMax, double *pdfMean,
                         double *padfStdDev) override;
    CPLErr ComputeStatistics(int bApproxOK, double *pdfMin, double *pdfMax,
                             double *pdfMean, double *pdfStdDev,
                             GDALProgressFunc, void *pProgressData) override;
    CPLErr SetStatistics(double dfMin, double dfMax, double dfMean,
                         double dfStdDev) override;
    CPLErr ComputeRasterMinMax(int, double *) override;

    int HasArbitraryOverviews() override;
    int GetOverviewCount() override;
    GDALRasterBand *GetOverview(int) override;
    GDALRasterBand *GetRasterSampleOverview(GUIntBig) override;
    CPLErr BuildOverviews(const char *, int, const int *, GDALProgressFunc,
                          void *, CSLConstList papszOptions) override;

    CPLErr AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
                      int nBufXSize, int nBufYSize, GDALDataType eDT,
                      char **papszOptions) override;

    CPLErr GetHistogram(double dfMin, double dfMax, int nBuckets,
                        GUIntBig *panHistogram, int bIncludeOutOfRange,
                        int bApproxOK, GDALProgressFunc,
                        void *pProgressData) override;

    CPLErr GetDefaultHistogram(double *pdfMin, double *pdfMax, int *pnBuckets,
                               GUIntBig **ppanHistogram, int bForce,
                               GDALProgressFunc, void *pProgressData) override;
    CPLErr SetDefaultHistogram(double dfMin, double dfMax, int nBuckets,
                               GUIntBig *panHistogram) override;

    GDALRasterAttributeTable *GetDefaultRAT() override;
    CPLErr SetDefaultRAT(const GDALRasterAttributeTable *) override;

    GDALRasterBand *GetMaskBand() override;
    int GetMaskFlags() override;
    CPLErr CreateMaskBand(int nFlags) override;
    bool IsMaskBand() const override;
    GDALMaskValueRange GetMaskValueRange() const override;

    CPLVirtualMem *GetVirtualMemAuto(GDALRWFlag eRWFlag, int *pnPixelSpace,
                                     GIntBig *pnLineSpace,
                                     char **papszOptions) override;

    CPLErr InterpolateAtPoint(double dfPixel, double dfLine,
                              GDALRIOResampleAlg eInterpolation,
                              double *pdfRealValue,
                              double *pdfImagValue) const override;

    void EnablePixelTypeSignedByteWarning(bool b) override;

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALProxyRasterBand)
};

/* ******************************************************************** */
/*                     GDALProxyPoolDataset                             */
/* ******************************************************************** */

typedef struct _GDALProxyPoolCacheEntry GDALProxyPoolCacheEntry;
class GDALProxyPoolRasterBand;

class CPL_DLL GDALProxyPoolDataset : public GDALProxyDataset
{
  private:
    GIntBig responsiblePID = -1;

    mutable char *pszProjectionRef = nullptr;
    mutable OGRSpatialReference *m_poSRS = nullptr;
    mutable OGRSpatialReference *m_poGCPSRS = nullptr;
    GDALGeoTransform m_gt{};
    bool m_bHasSrcSRS = false;
    bool m_bHasSrcGeoTransform = false;
    char *pszGCPProjection = nullptr;
    int nGCPCount = 0;
    GDAL_GCP *pasGCPList = nullptr;
    CPLHashSet *metadataSet = nullptr;
    CPLHashSet *metadataItemSet = nullptr;

    mutable GDALProxyPoolCacheEntry *cacheEntry = nullptr;
    char *m_pszOwner = nullptr;

    GDALDataset *RefUnderlyingDataset(bool bForceOpen) const;

    GDALProxyPoolDataset(const char *pszSourceDatasetDescription,
                         GDALAccess eAccess, int bShared, const char *pszOwner);

  protected:
    GDALDataset *RefUnderlyingDataset() const override;
    void
    UnrefUnderlyingDataset(GDALDataset *poUnderlyingDataset) const override;

    friend class GDALProxyPoolRasterBand;

  public:
    GDALProxyPoolDataset(const char *pszSourceDatasetDescription,
                         int nRasterXSize, int nRasterYSize,
                         GDALAccess eAccess = GA_ReadOnly, int bShared = FALSE,
                         const char *pszProjectionRef = nullptr,
                         const GDALGeoTransform *pGT = nullptr,
                         const char *pszOwner = nullptr);

    static GDALProxyPoolDataset *Create(const char *pszSourceDatasetDescription,
                                        CSLConstList papszOpenOptions = nullptr,
                                        GDALAccess eAccess = GA_ReadOnly,
                                        int bShared = FALSE,
                                        const char *pszOwner = nullptr);

    ~GDALProxyPoolDataset() override;

    void SetOpenOptions(CSLConstList papszOpenOptions);

    // If size (nBlockXSize&nBlockYSize) parameters is zero
    // they will be loaded when RefUnderlyingRasterBand function is called.
    // But in this case we cannot use them in other non-virtual methods before
    // RefUnderlyingRasterBand fist call.
    void AddSrcBandDescription(GDALDataType eDataType, int nBlockXSize,
                               int nBlockYSize);

    // Used by VRT SimpleSource to add a single GDALProxyPoolRasterBand while
    // keeping all other bands initialized to a nullptr. This is under the
    // assumption, VRT SimpleSource will not have to access any other bands than
    // the one added.
    void AddSrcBand(int nBand, GDALDataType eDataType, int nBlockXSize,
                    int nBlockYSize);
    CPLErr FlushCache(bool bAtClosing) override;

    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

    CPLErr GetGeoTransform(GDALGeoTransform &) const override;
    CPLErr SetGeoTransform(const GDALGeoTransform &) override;

    // Special behavior for the following methods : they return a pointer
    // data type, that must be cached by the proxy, so it doesn't become invalid
    // when the underlying object get closed.
    char **GetMetadata(const char *pszDomain) override;
    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain) override;

    void *GetInternalHandle(const char *pszRequest) override;

    const OGRSpatialReference *GetGCPSpatialRef() const override;
    const GDAL_GCP *GetGCPs() override;

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALProxyPoolDataset)
};

/* ******************************************************************** */
/*                  GDALProxyPoolRasterBand                             */
/* ******************************************************************** */

class GDALProxyPoolOverviewRasterBand;
class GDALProxyPoolMaskBand;

class CPL_DLL GDALProxyPoolRasterBand : public GDALProxyRasterBand
{
  private:
    CPLHashSet *metadataSet = nullptr;
    CPLHashSet *metadataItemSet = nullptr;
    char *pszUnitType = nullptr;
    char **papszCategoryNames = nullptr;
    GDALColorTable *poColorTable = nullptr;

    int nSizeProxyOverviewRasterBand = 0;
    GDALProxyPoolOverviewRasterBand **papoProxyOverviewRasterBand = nullptr;
    GDALProxyPoolMaskBand *poProxyMaskBand = nullptr;

  protected:
    GDALRasterBand *
    RefUnderlyingRasterBand(bool bForceOpen = true) const override;
    void UnrefUnderlyingRasterBand(
        GDALRasterBand *poUnderlyingRasterBand) const override;

    friend class GDALProxyPoolOverviewRasterBand;
    friend class GDALProxyPoolMaskBand;

  public:
    GDALProxyPoolRasterBand(GDALProxyPoolDataset *poDS, int nBand,
                            GDALDataType eDataType, int nBlockXSize,
                            int nBlockYSize);
    GDALProxyPoolRasterBand(GDALProxyPoolDataset *poDS,
                            GDALRasterBand *poUnderlyingRasterBand);
    ~GDALProxyPoolRasterBand() override;

    void AddSrcMaskBandDescription(GDALDataType eDataType, int nBlockXSize,
                                   int nBlockYSize);

    void AddSrcMaskBandDescriptionFromUnderlying();

    // Special behavior for the following methods : they return a pointer
    // data type, that must be cached by the proxy, so it doesn't become invalid
    // when the underlying object get closed.
    char **GetMetadata(const char *pszDomain) override;
    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain) override;
    char **GetCategoryNames() override;
    const char *GetUnitType() override;
    GDALColorTable *GetColorTable() override;
    GDALRasterBand *GetOverview(int) override;
    GDALRasterBand *
    GetRasterSampleOverview(GUIntBig nDesiredSamples) override;  // TODO
    GDALRasterBand *GetMaskBand() override;

    CPLErr FlushCache(bool bAtClosing) override;

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALProxyPoolRasterBand)
};

/* ******************************************************************** */
/*                  GDALProxyPoolOverviewRasterBand                     */
/* ******************************************************************** */

class GDALProxyPoolOverviewRasterBand : public GDALProxyPoolRasterBand
{
  private:
    GDALProxyPoolRasterBand *poMainBand = nullptr;
    int nOverviewBand = 0;

    mutable GDALRasterBand *poUnderlyingMainRasterBand = nullptr;
    mutable int nRefCountUnderlyingMainRasterBand = 0;

    CPL_DISALLOW_COPY_ASSIGN(GDALProxyPoolOverviewRasterBand)

  protected:
    GDALRasterBand *
    RefUnderlyingRasterBand(bool bForceOpen = true) const override;
    void UnrefUnderlyingRasterBand(
        GDALRasterBand *poUnderlyingRasterBand) const override;

  public:
    GDALProxyPoolOverviewRasterBand(GDALProxyPoolDataset *poDS,
                                    GDALRasterBand *poUnderlyingOverviewBand,
                                    GDALProxyPoolRasterBand *poMainBand,
                                    int nOverviewBand);
    ~GDALProxyPoolOverviewRasterBand() override;
};

/* ******************************************************************** */
/*                      GDALProxyPoolMaskBand                           */
/* ******************************************************************** */

class GDALProxyPoolMaskBand : public GDALProxyPoolRasterBand
{
  private:
    GDALProxyPoolRasterBand *poMainBand = nullptr;

    mutable GDALRasterBand *poUnderlyingMainRasterBand = nullptr;
    mutable int nRefCountUnderlyingMainRasterBand = 0;

    CPL_DISALLOW_COPY_ASSIGN(GDALProxyPoolMaskBand)

  protected:
    GDALRasterBand *
    RefUnderlyingRasterBand(bool bForceOpen = true) const override;
    void UnrefUnderlyingRasterBand(
        GDALRasterBand *poUnderlyingRasterBand) const override;

  public:
    GDALProxyPoolMaskBand(GDALProxyPoolDataset *poDS,
                          GDALRasterBand *poUnderlyingMaskBand,
                          GDALProxyPoolRasterBand *poMainBand);
    GDALProxyPoolMaskBand(GDALProxyPoolDataset *poDS,
                          GDALProxyPoolRasterBand *poMainBand,
                          GDALDataType eDataType, int nBlockXSize,
                          int nBlockYSize);
    ~GDALProxyPoolMaskBand() override;

    bool IsMaskBand() const override
    {
        return true;
    }
};

#endif

/* ******************************************************************** */
/*            C types and methods declarations                          */
/* ******************************************************************** */

CPL_C_START

typedef struct GDALProxyPoolDatasetHS *GDALProxyPoolDatasetH;

GDALProxyPoolDatasetH CPL_DLL GDALProxyPoolDatasetCreate(
    const char *pszSourceDatasetDescription, int nRasterXSize, int nRasterYSize,
    GDALAccess eAccess, int bShared, const char *pszProjectionRef,
    const double *padfGeoTransform);

void CPL_DLL
GDALProxyPoolDatasetDelete(GDALProxyPoolDatasetH hProxyPoolDataset);

void CPL_DLL GDALProxyPoolDatasetAddSrcBandDescription(
    GDALProxyPoolDatasetH hProxyPoolDataset, GDALDataType eDataType,
    int nBlockXSize, int nBlockYSize);

int CPL_DLL GDALGetMaxDatasetPoolSize(void);

CPL_C_END

#endif /* #ifndef DOXYGEN_SKIP */

#endif /* GDAL_PROXY_H_INCLUDED */
