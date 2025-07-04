/******************************************************************************
 *
 * Project:  EOSAT FAST Format reader
 * Purpose:  Reads Landsat FAST-L7A, IRS 1C/1D
 * Author:   Andrey Kiselev, dron@ak4719.spb.edu
 *
 ******************************************************************************
 * Copyright (c) 2002, Andrey Kiselev <dron@ak4719.spb.edu>
 * Copyright (c) 2007-2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/
#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_spatialref.h"
#include "rawdataset.h"

#include <algorithm>

// constexpr int ADM_STD_HEADER_SIZE = 4608;  // Format specification says it
constexpr int ADM_HEADER_SIZE = 5000;  // Should be 4608, but some vendors
                                       // ship broken large datasets.
constexpr size_t ADM_MIN_HEADER_SIZE = 1536;  // And sometimes it can be
                                              // even 1/3 of standard size.

static const char ACQUISITION_DATE[] = "ACQUISITION DATE";
constexpr int ACQUISITION_DATE_SIZE = 8;

static const char SATELLITE_NAME[] = "SATELLITE";
constexpr int SATELLITE_NAME_SIZE = 10;

static const char SENSOR_NAME[] = "SENSOR";
constexpr int SENSOR_NAME_SIZE = 10;

static const char BANDS_PRESENT[] = "BANDS PRESENT";
constexpr int BANDS_PRESENT_SIZE = 32;

static const char FILENAME[] = "FILENAME";
constexpr int FILENAME_SIZE = 29;

static const char PIXELS[] = "PIXELS PER LINE";
constexpr int PIXELS_SIZE = 5;

static const char LINES1[] = "LINES PER BAND";
static const char LINES2[] = "LINES PER IMAGE";
constexpr int LINES_SIZE = 5;

static const char BITS_PER_PIXEL[] = "OUTPUT BITS PER PIXEL";
constexpr int BITS_PER_PIXEL_SIZE = 2;

static const char PROJECTION_NAME[] = "MAP PROJECTION";
constexpr int PROJECTION_NAME_SIZE = 4;

static const char ELLIPSOID_NAME[] = "ELLIPSOID";
constexpr int ELLIPSOID_NAME_SIZE = 18;

static const char DATUM_NAME[] = "DATUM";
constexpr int DATUM_NAME_SIZE = 6;

static const char ZONE_NUMBER[] = "USGS MAP ZONE";
constexpr int ZONE_NUMBER_SIZE = 6;

static const char USGS_PARAMETERS[] = "USGS PROJECTION PARAMETERS";

static const char CORNER_UPPER_LEFT[] = "UL ";
static const char CORNER_UPPER_RIGHT[] = "UR ";
static const char CORNER_LOWER_LEFT[] = "LL ";
static const char CORNER_LOWER_RIGHT[] = "LR ";
constexpr int CORNER_VALUE_SIZE = 13;

constexpr int VALUE_SIZE = 24;

enum FASTSatellite  // Satellites:
{
    LANDSAT,  // Landsat 7
    IRS,      // IRS 1C/1D
    FAST_UNKNOWN
};

constexpr int MAX_FILES = 7;

/************************************************************************/
/* ==================================================================== */
/*                              FASTDataset                             */
/* ==================================================================== */
/************************************************************************/

class FASTDataset final : public GDALPamDataset
{
    GDALGeoTransform m_gt{};
    OGRSpatialReference m_oSRS{};

    VSILFILE *fpHeader;
    CPLString apoChannelFilenames[MAX_FILES];
    VSILFILE *fpChannels[MAX_FILES];
    const char *pszFilename;
    char *pszDirname;
    GDALDataType eDataType;
    FASTSatellite iSatellite;

    int OpenChannel(const char *pszFilename, int iBand);

    CPL_DISALLOW_COPY_ASSIGN(FASTDataset)

  public:
    FASTDataset();
    ~FASTDataset() override;

    static GDALDataset *Open(GDALOpenInfo *);

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;

    const OGRSpatialReference *GetSpatialRef() const override
    {
        return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
    }

    VSILFILE *FOpenChannel(const char *, int iBand, int iFASTBand);
    void TryEuromap_IRS_1C_1D_ChannelNameConvention(int &l_nBands);

    char **GetFileList() override;
};

/************************************************************************/
/* ==================================================================== */
/*                              FASTDataset                             */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                           FASTDataset()                              */
/************************************************************************/

FASTDataset::FASTDataset()
    : fpHeader(nullptr), pszFilename(nullptr), pszDirname(nullptr),
      eDataType(GDT_Unknown), iSatellite(FAST_UNKNOWN)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    // TODO: Why does this not work?
    //   fill( fpChannels, fpChannels + CPL_ARRAYSIZE(fpChannels), NULL );
    for (int i = 0; i < MAX_FILES; ++i)
        fpChannels[i] = nullptr;
}

/************************************************************************/
/*                            ~FASTDataset()                            */
/************************************************************************/

FASTDataset::~FASTDataset()

{
    FlushCache(true);

    CPLFree(pszDirname);
    for (int i = 0; i < MAX_FILES; i++)
        if (fpChannels[i])
            CPL_IGNORE_RET_VAL(VSIFCloseL(fpChannels[i]));
    if (fpHeader != nullptr)
        CPL_IGNORE_RET_VAL(VSIFCloseL(fpHeader));
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr FASTDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                             GetFileList()                            */
/************************************************************************/

char **FASTDataset::GetFileList()
{
    char **papszFileList = GDALPamDataset::GetFileList();

    for (int i = 0; i < 6; i++)
    {
        if (!apoChannelFilenames[i].empty())
            papszFileList =
                CSLAddString(papszFileList, apoChannelFilenames[i].c_str());
    }

    return papszFileList;
}

/************************************************************************/
/*                             OpenChannel()                            */
/************************************************************************/

int FASTDataset::OpenChannel(const char *pszFilenameIn, int iBand)
{
    CPLAssert(fpChannels[iBand] == nullptr);
    fpChannels[iBand] = VSIFOpenL(pszFilenameIn, "rb");
    if (fpChannels[iBand])
        apoChannelFilenames[iBand] = pszFilenameIn;
    return fpChannels[iBand] != nullptr;
}

/************************************************************************/
/*                             FOpenChannel()                           */
/************************************************************************/

VSILFILE *FASTDataset::FOpenChannel(const char *pszBandname, int iBand,
                                    int iFASTBand)
{
    std::string osChannelFilename;
    const std::string osPrefix = CPLGetBasenameSafe(pszFilename);
    const std::string osSuffix = CPLGetExtensionSafe(pszFilename);

    fpChannels[iBand] = nullptr;

    switch (iSatellite)
    {
        case LANDSAT:
            if (pszBandname && !EQUAL(pszBandname, ""))
            {
                osChannelFilename =
                    CPLFormCIFilenameSafe(pszDirname, pszBandname, nullptr);
                if (OpenChannel(osChannelFilename.c_str(), iBand))
                    break;
                osChannelFilename = CPLFormFilenameSafe(
                    pszDirname,
                    CPLSPrintf("%s.b%02d", osPrefix.c_str(), iFASTBand),
                    nullptr);
                CPL_IGNORE_RET_VAL(
                    OpenChannel(osChannelFilename.c_str(), iBand));
            }
            break;
        case IRS:
        default:
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("%s.%d", osPrefix.c_str(), iFASTBand),
                osSuffix.c_str());
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("IMAGERY%d", iFASTBand),
                osSuffix.c_str());
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("imagery%d", iFASTBand),
                osSuffix.c_str());
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("IMAGERY%d.DAT", iFASTBand), nullptr);
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("imagery%d.dat", iFASTBand), nullptr);
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("IMAGERY%d.dat", iFASTBand), nullptr);
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("imagery%d.DAT", iFASTBand), nullptr);
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("BAND%d", iFASTBand), osSuffix.c_str());
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("band%d", iFASTBand), osSuffix.c_str());
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("BAND%d.DAT", iFASTBand), nullptr);
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("band%d.dat", iFASTBand), nullptr);
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("BAND%d.dat", iFASTBand), nullptr);
            if (OpenChannel(osChannelFilename.c_str(), iBand))
                break;
            osChannelFilename = CPLFormFilenameSafe(
                pszDirname, CPLSPrintf("band%d.DAT", iFASTBand), nullptr);
            CPL_IGNORE_RET_VAL(OpenChannel(osChannelFilename.c_str(), iBand));
            break;
    }

    CPLDebug("FAST", "Band %d filename=%s", iBand + 1,
             osChannelFilename.c_str());

    return fpChannels[iBand];
}

/************************************************************************/
/*                TryEuromap_IRS_1C_1D_ChannelNameConvention()          */
/************************************************************************/

void FASTDataset::TryEuromap_IRS_1C_1D_ChannelNameConvention(int &l_nBands)
{
    // Filename convention explained in:
    // http://www.euromap.de/download/em_names.pdf

    char chLastLetterHeader = pszFilename[strlen(pszFilename) - 1];
    if (EQUAL(GetMetadataItem("SENSOR"), "PAN"))
    {
        /* Converting upper-case to lower case */
        if (chLastLetterHeader >= 'A' && chLastLetterHeader <= 'M')
            chLastLetterHeader += 'a' - 'A';

        if (chLastLetterHeader >= 'a' && chLastLetterHeader <= 'j')
        {
            const char chLastLetterData = chLastLetterHeader - 'a' + '0';
            char *pszChannelFilename = CPLStrdup(pszFilename);
            pszChannelFilename[strlen(pszChannelFilename) - 1] =
                chLastLetterData;
            if (OpenChannel(pszChannelFilename, 0))
                l_nBands++;
            else
                CPLDebug("FAST", "Could not find %s", pszChannelFilename);
            CPLFree(pszChannelFilename);
        }
        else if (chLastLetterHeader >= 'k' && chLastLetterHeader <= 'm')
        {
            const char chLastLetterData = chLastLetterHeader - 'k' + 'n';
            char *pszChannelFilename = CPLStrdup(pszFilename);
            pszChannelFilename[strlen(pszChannelFilename) - 1] =
                chLastLetterData;
            if (OpenChannel(pszChannelFilename, 0))
            {
                l_nBands++;
            }
            else
            {
                /* Trying upper-case */
                pszChannelFilename[strlen(pszChannelFilename) - 1] =
                    chLastLetterData - 'a' + 'A';
                if (OpenChannel(pszChannelFilename, 0))
                    l_nBands++;
                else
                    CPLDebug("FAST", "Could not find %s", pszChannelFilename);
            }
            CPLFree(pszChannelFilename);
        }
        else
        {
            CPLDebug(
                "FAST",
                "Unknown last letter (%c) for a IRS PAN Euromap FAST dataset",
                chLastLetterHeader);
        }
    }
    else if (EQUAL(GetMetadataItem("SENSOR"), "LISS3"))
    {
        const char apchLISSFilenames[7][5] = {
            {'0', '2', '3', '4', '5'}, {'6', '7', '8', '9', 'a'},
            {'b', 'c', 'd', 'e', 'f'}, {'g', 'h', 'i', 'j', 'k'},
            {'l', 'm', 'n', 'o', 'p'}, {'q', 'r', 's', 't', 'u'},
            {'v', 'w', 'x', 'y', 'z'}};

        int i = 0;
        for (; i < 7; i++)
        {
            if (chLastLetterHeader == apchLISSFilenames[i][0] ||
                (apchLISSFilenames[i][0] >= 'a' &&
                 apchLISSFilenames[i][0] <= 'z' &&
                 (apchLISSFilenames[i][0] - chLastLetterHeader == 0 ||
                  apchLISSFilenames[i][0] - chLastLetterHeader == 32)))
            {
                for (int j = 0; j < 4; j++)
                {
                    char *pszChannelFilename = CPLStrdup(pszFilename);
                    pszChannelFilename[strlen(pszChannelFilename) - 1] =
                        apchLISSFilenames[i][j + 1];
                    if (OpenChannel(pszChannelFilename, l_nBands))
                        l_nBands++;
                    else if (apchLISSFilenames[i][j + 1] >= 'a' &&
                             apchLISSFilenames[i][j + 1] <= 'z')
                    {
                        /* Trying upper-case */
                        pszChannelFilename[strlen(pszChannelFilename) - 1] =
                            apchLISSFilenames[i][j + 1] - 'a' + 'A';
                        if (OpenChannel(pszChannelFilename, l_nBands))
                        {
                            l_nBands++;
                        }
                        else
                        {
                            CPLDebug("FAST", "Could not find %s",
                                     pszChannelFilename);
                        }
                    }
                    else
                    {
                        CPLDebug("FAST", "Could not find %s",
                                 pszChannelFilename);
                    }
                    CPLFree(pszChannelFilename);
                }
                break;
            }
        }
        if (i == 7)
        {
            CPLDebug(
                "FAST",
                "Unknown last letter (%c) for a IRS LISS3 Euromap FAST dataset",
                chLastLetterHeader);
        }
    }
    else if (EQUAL(GetMetadataItem("SENSOR"), "WIFS"))
    {
        if (chLastLetterHeader == '0')
        {
            for (int j = 0; j < 2; j++)
            {
                char *pszChannelFilename = CPLStrdup(pszFilename);
                pszChannelFilename[strlen(pszChannelFilename) - 1] =
                    static_cast<char>('1' + j);
                if (OpenChannel(pszChannelFilename, l_nBands))
                {
                    l_nBands++;
                }
                else
                {
                    CPLDebug("FAST", "Could not find %s", pszChannelFilename);
                }
                CPLFree(pszChannelFilename);
            }
        }
        else
        {
            CPLDebug(
                "FAST",
                "Unknown last letter (%c) for a IRS WIFS Euromap FAST dataset",
                chLastLetterHeader);
        }
    }
    else
    {
        CPLAssert(false);
    }
}

/************************************************************************/
/*                          GetValue()                                  */
/************************************************************************/

static char *GetValue(const char *pszString, const char *pszName,
                      int iValueSize, int bNormalize)
{
    char *pszTemp = strstr(const_cast<char *>(pszString), pszName);
    if (pszTemp)
    {
        // Skip the parameter name
        pszTemp += strlen(pszName);
        // Skip whitespaces and equal signs
        while (*pszTemp == ' ')
            pszTemp++;
        while (*pszTemp == '=')
            pszTemp++;

        pszTemp = CPLScanString(pszTemp, iValueSize, TRUE, bNormalize);
    }

    return pszTemp;
}

/************************************************************************/
/*                        USGSMnemonicToCode()                          */
/************************************************************************/

static long USGSMnemonicToCode(const char *pszMnemonic)
{
    if (EQUAL(pszMnemonic, "UTM"))
        return 1L;
    else if (EQUAL(pszMnemonic, "LCC"))
        return 4L;
    else if (EQUAL(pszMnemonic, "PS"))
        return 6L;
    else if (EQUAL(pszMnemonic, "PC"))
        return 7L;
    else if (EQUAL(pszMnemonic, "TM"))
        return 9L;
    else if (EQUAL(pszMnemonic, "OM"))
        return 20L;
    else if (EQUAL(pszMnemonic, "SOM"))
        return 22L;
    else
        return 1L;  // UTM by default
}

/************************************************************************/
/*                        USGSEllipsoidToCode()                         */
/************************************************************************/

static long USGSEllipsoidToCode(const char *pszMnemonic)
{
    if (EQUAL(pszMnemonic, "CLARKE_1866"))
        return 0L;
    else if (EQUAL(pszMnemonic, "CLARKE_1880"))
        return 1L;
    else if (EQUAL(pszMnemonic, "BESSEL"))
        return 2L;
    else if (EQUAL(pszMnemonic, "INTERNATL_1967"))
        return 3L;
    else if (EQUAL(pszMnemonic, "INTERNATL_1909"))
        return 4L;
    else if (EQUAL(pszMnemonic, "WGS72") || EQUAL(pszMnemonic, "WGS_72"))
        return 5L;
    else if (EQUAL(pszMnemonic, "EVEREST"))
        return 6L;
    else if (EQUAL(pszMnemonic, "WGS66") || EQUAL(pszMnemonic, "WGS_66"))
        return 7L;
    else if (EQUAL(pszMnemonic, "GRS_80"))
        return 8L;
    else if (EQUAL(pszMnemonic, "AIRY"))
        return 9L;
    else if (EQUAL(pszMnemonic, "MODIFIED_EVEREST"))
        return 10L;
    else if (EQUAL(pszMnemonic, "MODIFIED_AIRY"))
        return 11L;
    else if (EQUAL(pszMnemonic, "WGS84") || EQUAL(pszMnemonic, "WGS_84"))
        return 12L;
    else if (EQUAL(pszMnemonic, "SOUTHEAST_ASIA"))
        return 13L;
    else if (EQUAL(pszMnemonic, "AUSTRALIAN_NATL"))
        return 14L;
    else if (EQUAL(pszMnemonic, "KRASSOVSKY"))
        return 15L;
    else if (EQUAL(pszMnemonic, "HOUGH"))
        return 16L;
    else if (EQUAL(pszMnemonic, "MERCURY_1960"))
        return 17L;
    else if (EQUAL(pszMnemonic, "MOD_MERC_1968"))
        return 18L;
    else if (EQUAL(pszMnemonic, "6370997_M_SPHERE"))
        return 19L;
    else
        return 0L;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *FASTDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (poOpenInfo->nHeaderBytes < 1024 || poOpenInfo->fpL == nullptr)
        return nullptr;

    if (!EQUALN(reinterpret_cast<const char *>(poOpenInfo->pabyHeader) + 52,
                "ACQUISITION DATE =", 18) &&
        !EQUALN(reinterpret_cast<const char *>(poOpenInfo->pabyHeader) + 36,
                "ACQUISITION DATE =", 18))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*  Create a corresponding GDALDataset.                                 */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<FASTDataset>();

    std::swap(poDS->fpHeader, poOpenInfo->fpL);

    poDS->pszFilename = poOpenInfo->pszFilename;
    poDS->pszDirname =
        CPLStrdup(CPLGetDirnameSafe(poOpenInfo->pszFilename).c_str());

    /* -------------------------------------------------------------------- */
    /*  Read the administrative record.                                     */
    /* -------------------------------------------------------------------- */
    std::string osHeader;
    osHeader.resize(ADM_HEADER_SIZE);

    size_t nBytesRead = 0;
    if (VSIFSeekL(poDS->fpHeader, 0, SEEK_SET) >= 0)
        nBytesRead =
            VSIFReadL(&osHeader[0], 1, ADM_HEADER_SIZE, poDS->fpHeader);
    if (nBytesRead < ADM_MIN_HEADER_SIZE)
    {
        CPLDebug("FAST", "Header file too short. Reading failed");
        return nullptr;
    }
    osHeader.resize(nBytesRead);
    const char *pszHeader = osHeader.c_str();

    // Read acquisition date
    {
        char *pszTemp =
            GetValue(pszHeader, ACQUISITION_DATE, ACQUISITION_DATE_SIZE, TRUE);
        if (pszTemp == nullptr)
        {
            CPLDebug("FAST", "Cannot get ACQUISITION_DATE, using empty value.");
            pszTemp = CPLStrdup("");
        }
        poDS->SetMetadataItem("ACQUISITION_DATE", pszTemp);
        CPLFree(pszTemp);
    }

    // Read satellite name (will read the first one only)
    {
        char *pszTemp =
            GetValue(pszHeader, SATELLITE_NAME, SATELLITE_NAME_SIZE, TRUE);
        if (pszTemp == nullptr)
        {
            CPLDebug("FAST", "Cannot get SATELLITE_NAME, using empty value.");
            pszTemp = CPLStrdup("");
        }
        poDS->SetMetadataItem("SATELLITE", pszTemp);
        if (STARTS_WITH_CI(pszTemp, "LANDSAT"))
            poDS->iSatellite = LANDSAT;
        // TODO(schwehr): Was this a bug that both are IRS?
        // else if ( STARTS_WITH_CI(pszTemp, "IRS") )
        //    poDS->iSatellite = IRS;
        else
            poDS->iSatellite =
                IRS;  // TODO(schwehr): Should this be FAST_UNKNOWN?
        CPLFree(pszTemp);
    }

    // Read sensor name (will read the first one only)
    {
        char *pszTemp =
            GetValue(pszHeader, SENSOR_NAME, SENSOR_NAME_SIZE, TRUE);
        if (pszTemp == nullptr)
        {
            CPLDebug("FAST", "Cannot get SENSOR_NAME, using empty value.");
            pszTemp = CPLStrdup("");
        }
        poDS->SetMetadataItem("SENSOR", pszTemp);
        CPLFree(pszTemp);
    }

    // Read filenames
    int l_nBands = 0;

    if (strstr(pszHeader, FILENAME) == nullptr)
    {
        if (strstr(pszHeader, "GENERATING AGENCY =EUROMAP"))
        {
            // If we don't find the FILENAME field, let's try with the Euromap
            // PAN / LISS3 / WIFS IRS filename convention.
            if ((EQUAL(poDS->GetMetadataItem("SATELLITE"), "IRS 1C") ||
                 EQUAL(poDS->GetMetadataItem("SATELLITE"), "IRS 1D")) &&
                (EQUAL(poDS->GetMetadataItem("SENSOR"), "PAN") ||
                 EQUAL(poDS->GetMetadataItem("SENSOR"), "LISS3") ||
                 EQUAL(poDS->GetMetadataItem("SENSOR"), "WIFS")))
            {
                poDS->TryEuromap_IRS_1C_1D_ChannelNameConvention(l_nBands);
            }
            else if (EQUAL(poDS->GetMetadataItem("SATELLITE"), "CARTOSAT-1") &&
                     (EQUAL(poDS->GetMetadataItem("SENSOR"), "FORE") ||
                      EQUAL(poDS->GetMetadataItem("SENSOR"), "AFT")))
            {
                // See appendix F in
                // http://www.euromap.de/download/p5fast_20050301.pdf
                const CPLString osSuffix =
                    CPLGetExtensionSafe(poDS->pszFilename);
                const char *papszBasenames[] = {"BANDF", "bandf", "BANDA",
                                                "banda"};
                for (int i = 0; i < 4; i++)
                {
                    const CPLString osChannelFilename = CPLFormFilenameSafe(
                        poDS->pszDirname, papszBasenames[i], osSuffix);
                    if (poDS->OpenChannel(osChannelFilename, 0))
                    {
                        l_nBands = 1;
                        break;
                    }
                }
            }
            else if (EQUAL(poDS->GetMetadataItem("SATELLITE"), "IRS P6"))
            {
                // If BANDS_PRESENT="2345", the file bands are "BAND2.DAT",
                // "BAND3.DAT", etc.
                char *pszTemp = GetValue(pszHeader, BANDS_PRESENT,
                                         BANDS_PRESENT_SIZE, TRUE);
                if (pszTemp)
                {
                    for (int i = 0; pszTemp[i] != '\0'; i++)
                    {
                        if (pszTemp[i] >= '2' && pszTemp[i] <= '5')
                        {
                            if (poDS->FOpenChannel(poDS->pszFilename, l_nBands,
                                                   pszTemp[i] - '0'))
                                l_nBands++;
                        }
                    }
                    CPLFree(pszTemp);
                }
            }
        }
    }

    // If the previous lookup for band files didn't success, fallback to the
    // standard way of finding them, either by the FILENAME field, either with
    // the usual patterns like bandX.dat, etc.
    if (!l_nBands)
    {
        const char *pszTemp = pszHeader;
        for (int i = 0; i < 7; i++)
        {
            char *pszFilename = nullptr;
            if (pszTemp)
                pszTemp = strstr(pszTemp, FILENAME);
            if (pszTemp)
            {
                // Skip the parameter name
                pszTemp += strlen(FILENAME);
                // Skip whitespaces and equal signs
                while (*pszTemp == ' ')
                    pszTemp++;
                while (*pszTemp == '=')
                    pszTemp++;
                pszFilename =
                    CPLScanString(pszTemp, FILENAME_SIZE, TRUE, FALSE);
            }
            else
                pszTemp = nullptr;
            if (poDS->FOpenChannel(pszFilename, l_nBands, l_nBands + 1))
                l_nBands++;
            if (pszFilename)
                CPLFree(pszFilename);
        }
    }

    if (!l_nBands)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Failed to find and open band data files.");
        return nullptr;
    }

    // Read number of pixels/lines and bit depth
    {
        char *pszTemp = GetValue(pszHeader, PIXELS, PIXELS_SIZE, FALSE);
        if (pszTemp)
        {
            poDS->nRasterXSize = atoi(pszTemp);
            CPLFree(pszTemp);
        }
        else
        {
            CPLDebug("FAST", "Failed to find number of pixels in line.");
            return nullptr;
        }
    }

    {
        char *pszTemp = GetValue(pszHeader, LINES1, LINES_SIZE, FALSE);
        if (!pszTemp)
            pszTemp = GetValue(pszHeader, LINES2, LINES_SIZE, FALSE);
        if (pszTemp)
        {
            poDS->nRasterYSize = atoi(pszTemp);
            CPLFree(pszTemp);
        }
        else
        {
            CPLDebug("FAST", "Failed to find number of lines in raster.");
            return nullptr;
        }
    }

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize))
    {
        return nullptr;
    }

    {
        char *pszTemp =
            GetValue(pszHeader, BITS_PER_PIXEL, BITS_PER_PIXEL_SIZE, FALSE);
        if (pszTemp)
        {
            switch (atoi(pszTemp))
            {
                case 8:
                default:
                    poDS->eDataType = GDT_Byte;
                    break;
                // For a strange reason, some Euromap products declare 10 bits
                // output, but are 16 bits.
                case 10:
                case 16:
                    poDS->eDataType = GDT_UInt16;
                    break;
            }
            CPLFree(pszTemp);
        }
        else
        {
            poDS->eDataType = GDT_Byte;
        }
    }

    /* -------------------------------------------------------------------- */
    /*  Read radiometric record.                                            */
    /* -------------------------------------------------------------------- */
    {
        const char *pszFirst = nullptr;
        const char *pszSecond = nullptr;

        // Read gains and biases. This is a trick!
        const char *pszTemp =
            strstr(pszHeader, "BIASES");  // It may be "BIASES AND GAINS"
                                          // or "GAINS AND BIASES"
        const char *pszGains = strstr(pszHeader, "GAINS");
        if (pszTemp == nullptr || pszGains == nullptr)
        {
            CPLDebug("FAST", "No BIASES and/or GAINS");
            return nullptr;
        }
        if (pszTemp > pszGains)
        {
            pszFirst = "GAIN%d";
            pszSecond = "BIAS%d";
        }
        else
        {
            pszFirst = "BIAS%d";
            pszSecond = "GAIN%d";
        }

        // Now search for the first number occurrence after that string.
        for (int i = 1; i <= l_nBands; i++)
        {
            char *pszValue = nullptr;
            size_t nValueLen = VALUE_SIZE;

            pszTemp = strpbrk(pszTemp, "-.0123456789");
            if (pszTemp)
            {
                nValueLen = strspn(pszTemp, "+-.0123456789");
                pszValue = CPLScanString(pszTemp, static_cast<int>(nValueLen),
                                         TRUE, TRUE);
                poDS->SetMetadataItem(CPLSPrintf(pszFirst, i), pszValue);
                CPLFree(pszValue);
            }
            else
            {
                return nullptr;
            }
            pszTemp += nValueLen;
            pszTemp = strpbrk(pszTemp, "-.0123456789");
            if (pszTemp)
            {
                nValueLen = strspn(pszTemp, "+-.0123456789");
                pszValue = CPLScanString(pszTemp, static_cast<int>(nValueLen),
                                         TRUE, TRUE);
                poDS->SetMetadataItem(CPLSPrintf(pszSecond, i), pszValue);
                CPLFree(pszValue);
            }
            else
            {
                return nullptr;
            }
            pszTemp += nValueLen;
        }
    }

    /* -------------------------------------------------------------------- */
    /*  Read geometric record.                                              */
    /* -------------------------------------------------------------------- */
    // Coordinates of pixel's centers
    double dfULX = 0.0;
    double dfULY = 0.0;
    double dfURX = 0.0;
    double dfURY = 0.0;
    double dfLLX = 0.0;
    double dfLLY = 0.0;
    double dfLRX = 0.0;
    double dfLRY = 0.0;

    // Read projection name
    long iProjSys = 0;
    {
        char *pszTemp =
            GetValue(pszHeader, PROJECTION_NAME, PROJECTION_NAME_SIZE, FALSE);
        if (pszTemp && !EQUAL(pszTemp, ""))
            iProjSys = USGSMnemonicToCode(pszTemp);
        else
            iProjSys = 1L;  // UTM by default
        CPLFree(pszTemp);
    }

    // Read ellipsoid name
    long iDatum = 0;  // Clarke, 1866 (NAD1927) by default.
    {
        char *pszTemp =
            GetValue(pszHeader, ELLIPSOID_NAME, ELLIPSOID_NAME_SIZE, FALSE);
        if (pszTemp && !EQUAL(pszTemp, ""))
            iDatum = USGSEllipsoidToCode(pszTemp);
        CPLFree(pszTemp);
    }

    // Read zone number.
    long iZone = 0;
    {
        char *pszTemp =
            GetValue(pszHeader, ZONE_NUMBER, ZONE_NUMBER_SIZE, FALSE);
        if (pszTemp && !EQUAL(pszTemp, ""))
            iZone = atoi(pszTemp);
        CPLFree(pszTemp);
    }

    // Read 15 USGS projection parameters
    double adfProjParams[15] = {0.0};
    {
        const char *pszTemp = strstr(pszHeader, USGS_PARAMETERS);
        if (pszTemp && !EQUAL(pszTemp, ""))
        {
            pszTemp += strlen(USGS_PARAMETERS);
            for (int i = 0; i < 15; i++)
            {
                pszTemp = strpbrk(pszTemp, "-.0123456789");
                if (pszTemp)
                {
                    adfProjParams[i] = CPLScanDouble(pszTemp, VALUE_SIZE);
                    pszTemp = strpbrk(pszTemp, " \t");
                }
                if (pszTemp == nullptr)
                {
                    return nullptr;
                }
            }
        }
    }

    // Coordinates should follow the word "PROJECTION", otherwise we can
    // be confused by other occurrences of the corner keywords.
    const char *pszGeomRecord = strstr(pszHeader, "PROJECTION");
    if (pszGeomRecord)
    {
        // Read corner coordinates
        const char *pszTemp = strstr(pszGeomRecord, CORNER_UPPER_LEFT);
        if (pszTemp && !EQUAL(pszTemp, "") &&
            strlen(pszTemp) >=
                strlen(CORNER_UPPER_LEFT) + 28 + CORNER_VALUE_SIZE + 1)
        {
            pszTemp += strlen(CORNER_UPPER_LEFT) + 28;
            dfULX = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
            pszTemp += CORNER_VALUE_SIZE + 1;
            dfULY = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
        }

        pszTemp = strstr(pszGeomRecord, CORNER_UPPER_RIGHT);
        if (pszTemp && !EQUAL(pszTemp, "") &&
            strlen(pszTemp) >=
                strlen(CORNER_UPPER_RIGHT) + 28 + CORNER_VALUE_SIZE + 1)
        {
            pszTemp += strlen(CORNER_UPPER_RIGHT) + 28;
            dfURX = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
            pszTemp += CORNER_VALUE_SIZE + 1;
            dfURY = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
        }

        pszTemp = strstr(pszGeomRecord, CORNER_LOWER_LEFT);
        if (pszTemp && !EQUAL(pszTemp, "") &&
            strlen(pszTemp) >=
                strlen(CORNER_LOWER_LEFT) + 28 + CORNER_VALUE_SIZE + 1)
        {
            pszTemp += strlen(CORNER_LOWER_LEFT) + 28;
            dfLLX = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
            pszTemp += CORNER_VALUE_SIZE + 1;
            dfLLY = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
        }

        pszTemp = strstr(pszGeomRecord, CORNER_LOWER_RIGHT);
        if (pszTemp && !EQUAL(pszTemp, "") &&
            strlen(pszTemp) >=
                strlen(CORNER_LOWER_RIGHT) + 28 + CORNER_VALUE_SIZE + 1)
        {
            pszTemp += strlen(CORNER_LOWER_RIGHT) + 28;
            dfLRX = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
            pszTemp += CORNER_VALUE_SIZE + 1;
            dfLRY = CPLScanDouble(pszTemp, CORNER_VALUE_SIZE);
        }
    }

    if (dfULX != 0.0 && dfULY != 0.0 && dfURX != 0.0 && dfURY != 0.0 &&
        dfLLX != 0.0 && dfLLY != 0.0 && dfLRX != 0.0 && dfLRY != 0.0)
    {
        // Strip out zone number from the easting values, if either
        if (dfULX >= 1000000.0)
            dfULX -= static_cast<double>(iZone) * 1000000.0;
        if (dfURX >= 1000000.0)
            dfURX -= static_cast<double>(iZone) * 1000000.0;
        if (dfLLX >= 1000000.0)
            dfLLX -= static_cast<double>(iZone) * 1000000.0;
        if (dfLRX >= 1000000.0)
            dfLRX -= static_cast<double>(iZone) * 1000000.0;

        // In EOSAT FAST Rev C, the angles are in decimal degrees
        // otherwise they are in packed DMS format.
        const int bAnglesInPackedDMSFormat =
            strstr(pszHeader, "REV            C") == nullptr;

        // Create projection definition
        OGRErr eErr = poDS->m_oSRS.importFromUSGS(
            iProjSys, iZone, adfProjParams, iDatum, bAnglesInPackedDMSFormat);
        if (eErr != OGRERR_NONE)
            CPLDebug("FAST", "Import projection from USGS failed: %d", eErr);
        else
        {
            poDS->m_oSRS.SetLinearUnits(SRS_UL_METER, 1.0);

            // Read datum name
            char *pszTemp =
                GetValue(pszHeader, DATUM_NAME, DATUM_NAME_SIZE, FALSE);
            if (pszTemp)
            {
                if (EQUAL(pszTemp, "WGS84"))
                    poDS->m_oSRS.SetWellKnownGeogCS("WGS84");
                else if (EQUAL(pszTemp, "NAD27"))
                    poDS->m_oSRS.SetWellKnownGeogCS("NAD27");
                else if (EQUAL(pszTemp, "NAD83"))
                    poDS->m_oSRS.SetWellKnownGeogCS("NAD83");
                CPLFree(pszTemp);
            }
            else
            {
                // Reasonable fallback
                poDS->m_oSRS.SetWellKnownGeogCS("WGS84");
            }
        }

        // Generate GCPs
        GDAL_GCP *pasGCPList =
            static_cast<GDAL_GCP *>(CPLCalloc(sizeof(GDAL_GCP), 4));
        GDALInitGCPs(4, pasGCPList);
        CPLFree(pasGCPList[0].pszId);
        CPLFree(pasGCPList[1].pszId);
        CPLFree(pasGCPList[2].pszId);
        CPLFree(pasGCPList[3].pszId);

        /* Let's order the GCP in TL, TR, BR, BL order to benefit from the */
        /* GDALGCPsToGeoTransform optimization */
        pasGCPList[0].pszId = CPLStrdup("UPPER_LEFT");
        pasGCPList[0].dfGCPX = dfULX;
        pasGCPList[0].dfGCPY = dfULY;
        pasGCPList[0].dfGCPZ = 0.0;
        pasGCPList[0].dfGCPPixel = 0.5;
        pasGCPList[0].dfGCPLine = 0.5;
        pasGCPList[1].pszId = CPLStrdup("UPPER_RIGHT");
        pasGCPList[1].dfGCPX = dfURX;
        pasGCPList[1].dfGCPY = dfURY;
        pasGCPList[1].dfGCPZ = 0.0;
        pasGCPList[1].dfGCPPixel = poDS->nRasterXSize - 0.5;
        pasGCPList[1].dfGCPLine = 0.5;
        pasGCPList[2].pszId = CPLStrdup("LOWER_RIGHT");
        pasGCPList[2].dfGCPX = dfLRX;
        pasGCPList[2].dfGCPY = dfLRY;
        pasGCPList[2].dfGCPZ = 0.0;
        pasGCPList[2].dfGCPPixel = poDS->nRasterXSize - 0.5;
        pasGCPList[2].dfGCPLine = poDS->nRasterYSize - 0.5;
        pasGCPList[3].pszId = CPLStrdup("LOWER_LEFT");
        pasGCPList[3].dfGCPX = dfLLX;
        pasGCPList[3].dfGCPY = dfLLY;
        pasGCPList[3].dfGCPZ = 0.0;
        pasGCPList[3].dfGCPPixel = 0.5;
        pasGCPList[3].dfGCPLine = poDS->nRasterYSize - 0.5;

        // Calculate transformation matrix, if accurate
        const bool transform_ok = CPL_TO_BOOL(
            GDALGCPsToGeoTransform(4, pasGCPList, poDS->m_gt.data(), 0));
        if (!transform_ok)
        {
            poDS->m_gt = GDALGeoTransform();
            poDS->m_oSRS.Clear();
        }

        GDALDeinitGCPs(4, pasGCPList);
        CPLFree(pasGCPList);
    }

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    const int nPixelOffset = GDALGetDataTypeSizeBytes(poDS->eDataType);
    const int nLineOffset = poDS->nRasterXSize * nPixelOffset;

    for (int i = 1; i <= l_nBands; i++)
    {
        auto poBand = RawRasterBand::Create(
            poDS.get(), i, poDS->fpChannels[i - 1], 0, nPixelOffset,
            nLineOffset, poDS->eDataType, RawRasterBand::NATIVE_BYTE_ORDER,
            RawRasterBand::OwnFP::NO);
        if (!poBand)
            return nullptr;
        poDS->SetBand(i, std::move(poBand));
    }

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    // opens overviews.
    poDS->oOvManager.Initialize(poDS.get(), poDS->pszFilename);

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("FAST");
        return nullptr;
    }

    return poDS.release();
}

/************************************************************************/
/*                        GDALRegister_FAST()                           */
/************************************************************************/

void GDALRegister_FAST()

{
    if (GDALGetDriverByName("FAST") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("FAST");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "EOSAT FAST Format");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/fast.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = FASTDataset::Open;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
