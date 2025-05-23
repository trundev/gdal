/******************************************************************************
 *
 * Project:  KML Translator
 * Purpose:  Implements OGRLIBKMLDriver
 * Author:   Brian Case, rush at winkey dot org
 *
 ******************************************************************************
 * Copyright (c) 2010, Brian Case
 * Copyright (c) 2010-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

#include "libkml_headers.h"
#include "ogrlibkmlfield.h"

#include <string>

#include "ogr_feature.h"
#include "ogr_p.h"
#include "ogrsf_frmts.h"

using kmldom::CameraPtr;
using kmldom::DataPtr;
using kmldom::ExtendedDataPtr;
using kmldom::FeaturePtr;
using kmldom::GeometryPtr;
using kmldom::GroundOverlayPtr;
using kmldom::GxMultiTrackPtr;
using kmldom::GxTrackPtr;
using kmldom::IconPtr;
using kmldom::KmlFactory;
using kmldom::LineStringPtr;
using kmldom::MultiGeometryPtr;
using kmldom::PlacemarkPtr;
using kmldom::PointPtr;
using kmldom::PolygonPtr;
using kmldom::SchemaDataPtr;
using kmldom::SchemaPtr;
using kmldom::SimpleDataPtr;
using kmldom::SimpleFieldPtr;
using kmldom::SnippetPtr;
using kmldom::TimePrimitivePtr;
using kmldom::TimeSpanPtr;
using kmldom::TimeStampPtr;

static void ogr2altitudemode_rec(const GeometryPtr &poKmlGeometry,
                                 int iAltitudeMode, int isGX)
{
    switch (poKmlGeometry->Type())
    {
        case kmldom::Type_Point:
        {
            PointPtr poKmlPoint = AsPoint(poKmlGeometry);

            if (!isGX)
                poKmlPoint->set_altitudemode(iAltitudeMode);
            else
                poKmlPoint->set_gx_altitudemode(iAltitudeMode);

            break;
        }
        case kmldom::Type_LineString:
        {
            LineStringPtr poKmlLineString = AsLineString(poKmlGeometry);

            if (!isGX)
                poKmlLineString->set_altitudemode(iAltitudeMode);
            else
                poKmlLineString->set_gx_altitudemode(iAltitudeMode);

            break;
        }
        case kmldom::Type_LinearRing:
        {
            break;
        }
        case kmldom::Type_Polygon:
        {
            PolygonPtr poKmlPolygon = AsPolygon(poKmlGeometry);

            if (!isGX)
                poKmlPolygon->set_altitudemode(iAltitudeMode);
            else
                poKmlPolygon->set_gx_altitudemode(iAltitudeMode);

            break;
        }
        case kmldom::Type_MultiGeometry:
        {
            MultiGeometryPtr poKmlMultiGeometry =
                AsMultiGeometry(poKmlGeometry);

            const size_t nGeom = poKmlMultiGeometry->get_geometry_array_size();
            for (size_t i = 0; i < nGeom; i++)
            {
                ogr2altitudemode_rec(
                    poKmlMultiGeometry->get_geometry_array_at(i), iAltitudeMode,
                    isGX);
            }

            break;
        }
        default:
        {
            break;
        }
    }
}

static void ogr2extrude_rec(bool bExtrude, const GeometryPtr &poKmlGeometry)
{
    switch (poKmlGeometry->Type())
    {
        case kmldom::Type_Point:
        {
            PointPtr const poKmlPoint = AsPoint(poKmlGeometry);
            poKmlPoint->set_extrude(bExtrude);
            break;
        }
        case kmldom::Type_LineString:
        {
            LineStringPtr const poKmlLineString = AsLineString(poKmlGeometry);
            poKmlLineString->set_extrude(bExtrude);
            break;
        }
        case kmldom::Type_LinearRing:
        {
            break;
        }
        case kmldom::Type_Polygon:
        {
            PolygonPtr const poKmlPolygon = AsPolygon(poKmlGeometry);
            poKmlPolygon->set_extrude(bExtrude);
            break;
        }
        case kmldom::Type_MultiGeometry:
        {
            MultiGeometryPtr const poKmlMultiGeometry =
                AsMultiGeometry(poKmlGeometry);

            const size_t nGeom = poKmlMultiGeometry->get_geometry_array_size();
            for (size_t i = 0; i < nGeom; i++)
            {
                ogr2extrude_rec(bExtrude,
                                poKmlMultiGeometry->get_geometry_array_at(i));
            }
            break;
        }
        default:
        {
            break;
        }
    }
}

static void ogr2tessellate_rec(bool bTessellate,
                               const GeometryPtr &poKmlGeometry)
{
    switch (poKmlGeometry->Type())
    {
        case kmldom::Type_Point:
        {
            break;
        }
        case kmldom::Type_LineString:
        {
            LineStringPtr const poKmlLineString = AsLineString(poKmlGeometry);
            poKmlLineString->set_tessellate(bTessellate);
            break;
        }
        case kmldom::Type_LinearRing:
        {
            break;
        }
        case kmldom::Type_Polygon:
        {
            PolygonPtr const poKmlPolygon = AsPolygon(poKmlGeometry);

            poKmlPolygon->set_tessellate(bTessellate);
            break;
        }
        case kmldom::Type_MultiGeometry:
        {
            MultiGeometryPtr const poKmlMultiGeometry =
                AsMultiGeometry(poKmlGeometry);

            const size_t nGeom = poKmlMultiGeometry->get_geometry_array_size();
            for (size_t i = 0; i < nGeom; i++)
            {
                ogr2tessellate_rec(
                    bTessellate, poKmlMultiGeometry->get_geometry_array_at(i));
            }

            break;
        }
        default:
        {
            break;
        }
    }
}

/************************************************************************/
/*                 OGRLIBKMLSanitizeUTF8String()                        */
/************************************************************************/

static char *OGRLIBKMLSanitizeUTF8String(const char *pszString)
{
    if (!CPLIsUTF8(pszString, -1) &&
        CPLTestBool(CPLGetConfigOption("OGR_FORCE_ASCII", "YES")))
    {
        static bool bFirstTime = true;
        if (bFirstTime)
        {
            bFirstTime = false;
            CPLError(
                CE_Warning, CPLE_AppDefined,
                "%s is not a valid UTF-8 string. Forcing it to ASCII.  "
                "If you still want the original string and change the XML file "
                "encoding afterwards, you can define OGR_FORCE_ASCII=NO as "
                "  configuration option.  This warning won't be issued anymore",
                pszString);
        }
        else
        {
            CPLDebug("OGR",
                     "%s is not a valid UTF-8 string. Forcing it to ASCII",
                     pszString);
        }
        return CPLForceToASCII(pszString, -1, '?');
    }

    return CPLStrdup(pszString);
}

/******************************************************************************
 Function to output ogr fields in kml.

 Args:
        poOgrFeat       pointer to the feature the field is in
        poOgrLayer      pointer to the layer the feature is in
        poKmlFactory    pointer to the libkml dom factory
        poKmlPlacemark  pointer to the placemark to add to

 Returns:
        nothing

 env vars:
  LIBKML_TIMESTAMP_FIELD         default: OFTDate or OFTDateTime named timestamp
  LIBKML_TIMESPAN_BEGIN_FIELD    default: OFTDate or OFTDateTime named begin
  LIBKML_TIMESPAN_END_FIELD      default: OFTDate or OFTDateTime named end
  LIBKML_DESCRIPTION_FIELD       default: none
  LIBKML_NAME_FIELD              default: OFTString field named name

******************************************************************************/

void field2kml(OGRFeature *poOgrFeat, OGRLIBKMLLayer *poOgrLayer,
               KmlFactory *poKmlFactory, FeaturePtr poKmlFeature,
               int bUseSimpleFieldIn, const fieldconfig &oFC)
{
    const bool bUseSimpleField = CPL_TO_BOOL(bUseSimpleFieldIn);
    SchemaDataPtr poKmlSchemaData = nullptr;
    if (bUseSimpleField)
    {
        poKmlSchemaData = poKmlFactory->CreateSchemaData();
        SchemaPtr poKmlSchema = poOgrLayer->GetKmlSchema();

        /***** set the url to the schema *****/
        if (poKmlSchema && poKmlSchema->has_id())
        {
            std::string oKmlSchemaID = poKmlSchema->get_id();
            std::string oKmlSchemaURL = "#";
            oKmlSchemaURL.append(oKmlSchemaID);

            poKmlSchemaData->set_schemaurl(oKmlSchemaURL);
        }
    }

    TimeSpanPtr poKmlTimeSpan = nullptr;

    const int nFields = poOgrFeat->GetFieldCount();
    int iSkip1 = -1;
    int iSkip2 = -1;
    int iAltitudeMode = kmldom::ALTITUDEMODE_CLAMPTOGROUND;
    int isGX = false;

    ExtendedDataPtr poKmlExtendedData = nullptr;

    for (int i = 0; i < nFields; i++)
    {
        /***** If the field is set to skip, do so *****/
        if (i == iSkip1 || i == iSkip2)
            continue;

        /***** If the field isn't set just bail now *****/
        if (!poOgrFeat->IsFieldSetAndNotNull(i))
            continue;

        const OGRFieldDefn *poOgrFieldDef = poOgrFeat->GetFieldDefnRef(i);
        const OGRFieldType type = poOgrFieldDef->GetType();
        const char *name = poOgrFieldDef->GetNameRef();

        SimpleDataPtr poKmlSimpleData = nullptr;
        DataPtr poKmlData = nullptr;
        OGRField sFieldDT;

        // TODO(schwehr): Refactor to get rid of gotos.
        switch (type)
        {
            case OFTString:  //     String of ASCII chars
            {
                char *const pszUTF8String =
                    OGRLIBKMLSanitizeUTF8String(poOgrFeat->GetFieldAsString(i));
                if (pszUTF8String[0] == '\0')
                {
                    CPLFree(pszUTF8String);
                    continue;
                }

                /***** id *****/
                if (EQUAL(name, oFC.idfield))
                {
                    poKmlFeature->set_id(pszUTF8String);
                    CPLFree(pszUTF8String);
                    continue;
                }
                /***** name *****/
                if (EQUAL(name, oFC.namefield))
                {
                    poKmlFeature->set_name(pszUTF8String);
                    CPLFree(pszUTF8String);
                    continue;
                }
                /***** description *****/
                else if (EQUAL(name, oFC.descfield))
                {
                    poKmlFeature->set_description(pszUTF8String);
                    CPLFree(pszUTF8String);
                    continue;
                }
                /***** altitudemode *****/
                else if (EQUAL(name, oFC.altitudeModefield))
                {
                    const char *pszAltitudeMode = pszUTF8String;

                    iAltitudeMode =
                        kmlAltitudeModeFromString(pszAltitudeMode, isGX);

                    if (poKmlFeature->IsA(kmldom::Type_Placemark))
                    {
                        PlacemarkPtr const poKmlPlacemark =
                            AsPlacemark(poKmlFeature);
                        if (poKmlPlacemark->has_geometry())
                        {
                            GeometryPtr poKmlGeometry =
                                poKmlPlacemark->get_geometry();

                            ogr2altitudemode_rec(poKmlGeometry, iAltitudeMode,
                                                 isGX);
                        }
                    }

                    CPLFree(pszUTF8String);

                    continue;
                }
                /***** timestamp *****/
                else if (EQUAL(name, oFC.tsfield))
                {
                    TimeStampPtr poKmlTimeStamp =
                        poKmlFactory->CreateTimeStamp();
                    poKmlTimeStamp->set_when(pszUTF8String);
                    poKmlFeature->set_timeprimitive(poKmlTimeStamp);

                    CPLFree(pszUTF8String);

                    continue;
                }
                /***** begin *****/
                if (EQUAL(name, oFC.beginfield))
                {
                    if (!poKmlTimeSpan)
                    {
                        poKmlTimeSpan = poKmlFactory->CreateTimeSpan();
                        poKmlFeature->set_timeprimitive(poKmlTimeSpan);
                    }

                    poKmlTimeSpan->set_begin(pszUTF8String);

                    CPLFree(pszUTF8String);

                    continue;
                }
                /***** end *****/
                else if (EQUAL(name, oFC.endfield))
                {
                    if (!poKmlTimeSpan)
                    {
                        poKmlTimeSpan = poKmlFactory->CreateTimeSpan();
                        poKmlFeature->set_timeprimitive(poKmlTimeSpan);
                    }

                    poKmlTimeSpan->set_end(pszUTF8String);

                    CPLFree(pszUTF8String);

                    continue;
                }
                /***** snippet *****/
                else if (EQUAL(name, oFC.snippetfield))
                {
                    SnippetPtr snippet = poKmlFactory->CreateSnippet();
                    snippet->set_text(pszUTF8String);
                    poKmlFeature->set_snippet(snippet);

                    CPLFree(pszUTF8String);

                    continue;
                }
                /***** other special fields *****/
                else if (EQUAL(name, oFC.iconfield) ||
                         EQUAL(name, oFC.modelfield) ||
                         EQUAL(name, oFC.networklinkfield) ||
                         EQUAL(name, oFC.networklink_refreshMode_field) ||
                         EQUAL(name, oFC.networklink_viewRefreshMode_field) ||
                         EQUAL(name, oFC.networklink_viewFormat_field) ||
                         EQUAL(name, oFC.networklink_httpQuery_field) ||
                         EQUAL(name, oFC.camera_altitudemode_field) ||
                         EQUAL(name, oFC.photooverlayfield) ||
                         EQUAL(name, oFC.photooverlay_shape_field) ||
                         EQUAL(name, oFC.imagepyramid_gridorigin_field))
                {
                    CPLFree(pszUTF8String);

                    continue;
                }

                /***** other *****/

                if (bUseSimpleField)
                {
                    poKmlSimpleData = poKmlFactory->CreateSimpleData();
                    poKmlSimpleData->set_name(name);
                    poKmlSimpleData->set_text(pszUTF8String);
                }
                else
                {
                    poKmlData = poKmlFactory->CreateData();
                    poKmlData->set_name(name);
                    poKmlData->set_value(pszUTF8String);
                }

                CPLFree(pszUTF8String);

                break;
            }

            // This code checks if there's a OFTTime field with the same name
            // that could be used to compose a DateTime. Not sure this is really
            // supported in OGR data model to have 2 fields with same name.
            case OFTDate:  //   Date
            {
                memcpy(&sFieldDT, poOgrFeat->GetRawFieldRef(i),
                       sizeof(OGRField));

                for (int iTimeField = i + 1; iTimeField < nFields; iTimeField++)
                {
                    if (iTimeField == iSkip1 || iTimeField == iSkip2)
                        continue;

                    OGRFieldDefn *poOgrFieldDef2 =
                        poOgrFeat->GetFieldDefnRef(i);
                    OGRFieldType type2 = poOgrFieldDef2->GetType();
                    const char *name2 = poOgrFieldDef2->GetNameRef();

                    if (EQUAL(name2, name) && type2 == OFTTime &&
                        (EQUAL(name, oFC.tsfield) ||
                         EQUAL(name, oFC.beginfield) ||
                         EQUAL(name, oFC.endfield)))
                    {
                        const OGRField *const psField2 =
                            poOgrFeat->GetRawFieldRef(iTimeField);
                        sFieldDT.Date.Hour = psField2->Date.Hour;
                        sFieldDT.Date.Minute = psField2->Date.Minute;
                        sFieldDT.Date.Second = psField2->Date.Second;
                        sFieldDT.Date.TZFlag = psField2->Date.TZFlag;

                        if (0 > iSkip1)
                            iSkip1 = iTimeField;
                        else
                            iSkip2 = iTimeField;
                    }
                }

                goto Do_DateTime;
            }

            // This code checks if there's a OFTTime field with the same name
            // that could be used to compose a DateTime. Not sure this is really
            // supported in OGR data model to have 2 fields with same name.
            case OFTTime:  //   Time
            {
                memcpy(&sFieldDT, poOgrFeat->GetRawFieldRef(i),
                       sizeof(OGRField));

                for (int iTimeField = i + 1; iTimeField < nFields; iTimeField++)
                {
                    if (iTimeField == iSkip1 || iTimeField == iSkip2)
                        continue;

                    OGRFieldDefn *const poOgrFieldDef2 =
                        poOgrFeat->GetFieldDefnRef(i);
                    OGRFieldType type2 = poOgrFieldDef2->GetType();
                    const char *const name2 = poOgrFieldDef2->GetNameRef();

                    if (EQUAL(name2, name) && type2 == OFTDate &&
                        (EQUAL(name, oFC.tsfield) ||
                         EQUAL(name, oFC.beginfield) ||
                         EQUAL(name, oFC.endfield)))
                    {
                        const OGRField *psField2 =
                            poOgrFeat->GetRawFieldRef(iTimeField);
                        sFieldDT.Date.Year = psField2->Date.Year;
                        sFieldDT.Date.Month = psField2->Date.Month;
                        sFieldDT.Date.Day = psField2->Date.Day;

                        if (0 > iSkip1)
                            iSkip1 = iTimeField;
                        else
                            iSkip2 = iTimeField;
                    }
                }

                goto Do_DateTime;
            }

            case OFTDateTime:  //  Date and Time
            {
                memcpy(&sFieldDT, poOgrFeat->GetRawFieldRef(i),
                       sizeof(OGRField));

            Do_DateTime:
                /***** timestamp *****/
                if (EQUAL(name, oFC.tsfield))
                {
                    char *const timebuf = OGRGetXMLDateTime(&sFieldDT);

                    TimeStampPtr poKmlTimeStamp =
                        poKmlFactory->CreateTimeStamp();
                    poKmlTimeStamp->set_when(timebuf);
                    poKmlFeature->set_timeprimitive(poKmlTimeStamp);
                    CPLFree(timebuf);

                    continue;
                }

                /***** begin *****/
                if (EQUAL(name, oFC.beginfield))
                {
                    char *const timebuf = OGRGetXMLDateTime(&sFieldDT);

                    if (!poKmlTimeSpan)
                    {
                        poKmlTimeSpan = poKmlFactory->CreateTimeSpan();
                        poKmlFeature->set_timeprimitive(poKmlTimeSpan);
                    }

                    poKmlTimeSpan->set_begin(timebuf);
                    CPLFree(timebuf);

                    continue;
                }

                /***** end *****/
                else if (EQUAL(name, oFC.endfield))
                {
                    char *const timebuf = OGRGetXMLDateTime(&sFieldDT);

                    if (!poKmlTimeSpan)
                    {
                        poKmlTimeSpan = poKmlFactory->CreateTimeSpan();
                        poKmlFeature->set_timeprimitive(poKmlTimeSpan);
                    }

                    poKmlTimeSpan->set_end(timebuf);
                    CPLFree(timebuf);

                    continue;
                }

                /***** other *****/
                const char *pszVal =
                    type == OFTDateTime
                        ? poOgrFeat->GetFieldAsISO8601DateTime(i, nullptr)
                        : poOgrFeat->GetFieldAsString(i);
                if (bUseSimpleField)
                {
                    poKmlSimpleData = poKmlFactory->CreateSimpleData();
                    poKmlSimpleData->set_name(name);
                    poKmlSimpleData->set_text(pszVal);
                }
                else
                {
                    poKmlData = poKmlFactory->CreateData();
                    poKmlData->set_name(name);
                    poKmlData->set_value(pszVal);
                }

                break;
            }

            case OFTInteger:  //    Simple 32bit integer
            {
                /***** extrude *****/
                if (EQUAL(name, oFC.extrudefield))
                {
                    if (poKmlFeature->IsA(kmldom::Type_Placemark))
                    {
                        PlacemarkPtr poKmlPlacemark = AsPlacemark(poKmlFeature);
                        if (poKmlPlacemark->has_geometry() &&
                            -1 < poOgrFeat->GetFieldAsInteger(i))
                        {
                            const int iExtrude =
                                poOgrFeat->GetFieldAsInteger(i);
                            if (iExtrude && !isGX &&
                                iAltitudeMode ==
                                    kmldom::ALTITUDEMODE_CLAMPTOGROUND &&
                                CPLTestBool(CPLGetConfigOption(
                                    "LIBKML_STRICT_COMPLIANCE", "TRUE")))
                            {
                                CPLError(CE_Warning, CPLE_NotSupported,
                                         "altitudeMode=clampToGround "
                                         "unsupported with "
                                         "extrude=1");
                            }
                            else
                            {
                                GeometryPtr poKmlGeometry =
                                    poKmlPlacemark->get_geometry();
                                ogr2extrude_rec(CPL_TO_BOOL(iExtrude),
                                                poKmlGeometry);
                            }
                        }
                    }
                    continue;
                }

                /***** tessellate *****/
                if (EQUAL(name, oFC.tessellatefield))
                {
                    if (poKmlFeature->IsA(kmldom::Type_Placemark))
                    {
                        PlacemarkPtr poKmlPlacemark = AsPlacemark(poKmlFeature);
                        if (poKmlPlacemark->has_geometry() &&
                            -1 < poOgrFeat->GetFieldAsInteger(i))
                        {
                            const int iTessellate =
                                poOgrFeat->GetFieldAsInteger(i);
                            if (iTessellate &&
                                !(!isGX &&
                                  static_cast<kmldom::AltitudeModeEnum>(
                                      iAltitudeMode) ==
                                      kmldom::ALTITUDEMODE_CLAMPTOGROUND) &&
                                !(isGX &&
                                  static_cast<kmldom::GxAltitudeModeEnum>(
                                      iAltitudeMode) ==
                                      kmldom::
                                          GX_ALTITUDEMODE_CLAMPTOSEAFLOOR) &&
                                CPLTestBool(CPLGetConfigOption(
                                    "LIBKML_STRICT_COMPLIANCE", "TRUE")))
                            {
                                CPLError(CE_Warning, CPLE_NotSupported,
                                         "altitudeMode!=clampToGround && "
                                         "altitudeMode!=clampToSeaFloor "
                                         "unsupported with tessellate=1");
                            }
                            else
                            {
                                GeometryPtr poKmlGeometry =
                                    poKmlPlacemark->get_geometry();
                                ogr2tessellate_rec(CPL_TO_BOOL(iTessellate),
                                                   poKmlGeometry);
                                if (!isGX &&
                                    iAltitudeMode ==
                                        kmldom::ALTITUDEMODE_CLAMPTOGROUND)
                                    ogr2altitudemode_rec(poKmlGeometry,
                                                         iAltitudeMode, isGX);
                            }
                        }
                    }

                    continue;
                }

                /***** visibility *****/
                if (EQUAL(name, oFC.visibilityfield))
                {
                    if (-1 < poOgrFeat->GetFieldAsInteger(i))
                        poKmlFeature->set_visibility(
                            CPL_TO_BOOL(poOgrFeat->GetFieldAsInteger(i)));

                    continue;
                }
                /***** other special fields *****/
                else if (EQUAL(name, oFC.drawOrderfield) ||
                         EQUAL(name, oFC.networklink_refreshvisibility_field) ||
                         EQUAL(name, oFC.networklink_flytoview_field) ||
                         EQUAL(name, oFC.networklink_refreshInterval_field) ||
                         EQUAL(name, oFC.networklink_viewRefreshMode_field) ||
                         EQUAL(name, oFC.networklink_viewRefreshTime_field) ||
                         EQUAL(name, oFC.imagepyramid_tilesize_field) ||
                         EQUAL(name, oFC.imagepyramid_maxwidth_field) ||
                         EQUAL(name, oFC.imagepyramid_maxheight_field))
                {
                    continue;
                }

                /***** other *****/
                const char *value =
                    poOgrFieldDef->GetSubType() == OFSTBoolean
                        ? (poOgrFeat->GetFieldAsInteger(i) ? "true" : "false")
                        : poOgrFeat->GetFieldAsString(i);
                if (bUseSimpleField)
                {
                    poKmlSimpleData = poKmlFactory->CreateSimpleData();
                    poKmlSimpleData->set_name(name);
                    poKmlSimpleData->set_text(value);
                }
                else
                {
                    poKmlData = poKmlFactory->CreateData();
                    poKmlData->set_name(name);
                    poKmlData->set_value(value);
                }

                break;
            }

            case OFTReal:  //   Double Precision floating point
            {
                if (EQUAL(name, oFC.headingfield) ||
                    EQUAL(name, oFC.tiltfield) || EQUAL(name, oFC.rollfield) ||
                    EQUAL(name, oFC.scalexfield) ||
                    EQUAL(name, oFC.scaleyfield) ||
                    EQUAL(name, oFC.scalezfield) ||
                    EQUAL(name, oFC.networklink_refreshInterval_field) ||
                    EQUAL(name, oFC.networklink_viewRefreshMode_field) ||
                    EQUAL(name, oFC.networklink_viewRefreshTime_field) ||
                    EQUAL(name, oFC.networklink_viewBoundScale_field) ||
                    EQUAL(name, oFC.camera_longitude_field) ||
                    EQUAL(name, oFC.camera_latitude_field) ||
                    EQUAL(name, oFC.camera_altitude_field) ||
                    EQUAL(name, oFC.leftfovfield) ||
                    EQUAL(name, oFC.rightfovfield) ||
                    EQUAL(name, oFC.bottomfovfield) ||
                    EQUAL(name, oFC.topfovfield) ||
                    EQUAL(name, oFC.nearfield) ||
                    EQUAL(name, oFC.camera_altitude_field))
                {
                    continue;
                }

                char *pszStr = CPLStrdup(poOgrFeat->GetFieldAsString(i));

                if (bUseSimpleField)
                {
                    poKmlSimpleData = poKmlFactory->CreateSimpleData();
                    poKmlSimpleData->set_name(name);
                    poKmlSimpleData->set_text(pszStr);
                }
                else
                {
                    poKmlData = poKmlFactory->CreateData();
                    poKmlData->set_name(name);
                    poKmlData->set_value(pszStr);
                }

                CPLFree(pszStr);

                break;
            }

            case OFTStringList:      //     Array of strings
            case OFTIntegerList:     //    List of 32bit integers
            case OFTRealList:        //    List of doubles
            case OFTBinary:          //     Raw Binary data
            case OFTWideStringList:  //     deprecated
            default:

                /***** other *****/

                if (bUseSimpleField)
                {
                    poKmlSimpleData = poKmlFactory->CreateSimpleData();
                    poKmlSimpleData->set_name(name);
                    poKmlSimpleData->set_text(poOgrFeat->GetFieldAsString(i));
                }
                else
                {
                    poKmlData = poKmlFactory->CreateData();
                    poKmlData->set_name(name);
                    poKmlData->set_value(poOgrFeat->GetFieldAsString(i));
                }

                break;
        }

        if (poKmlSimpleData)
        {
            poKmlSchemaData->add_simpledata(poKmlSimpleData);
        }
        else if (poKmlData)
        {
            if (!poKmlExtendedData)
                poKmlExtendedData = poKmlFactory->CreateExtendedData();
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
            poKmlExtendedData->add_data(poKmlData);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        }
    }

    // Do not add it to the placemark unless there is data.
    if (bUseSimpleField && poKmlSchemaData->get_simpledata_array_size() > 0)
    {
        poKmlExtendedData = poKmlFactory->CreateExtendedData();
        poKmlExtendedData->add_schemadata(poKmlSchemaData);
    }
    if (poKmlExtendedData)
    {
        poKmlFeature->set_extendeddata(poKmlExtendedData);
    }
}

/******************************************************************************
 Recursive function to read altitude mode from the geometry.
******************************************************************************/

static bool kml2altitudemode_rec(GeometryPtr poKmlGeometry, int *pnAltitudeMode,
                                 int *pbIsGX)
{
    switch (poKmlGeometry->Type())
    {
        case kmldom::Type_Point:
        {
            PointPtr poKmlPoint = AsPoint(poKmlGeometry);

            if (poKmlPoint->has_altitudemode())
            {
                *pnAltitudeMode = poKmlPoint->get_altitudemode();
                return true;
            }
            else if (poKmlPoint->has_gx_altitudemode())
            {
                *pnAltitudeMode = poKmlPoint->get_gx_altitudemode();
                *pbIsGX = true;
                return true;
            }

            break;
        }
        case kmldom::Type_LineString:
        {
            LineStringPtr poKmlLineString = AsLineString(poKmlGeometry);

            if (poKmlLineString->has_altitudemode())
            {
                *pnAltitudeMode = poKmlLineString->get_altitudemode();
                return true;
            }
            else if (poKmlLineString->has_gx_altitudemode())
            {
                *pnAltitudeMode = poKmlLineString->get_gx_altitudemode();
                *pbIsGX = true;
                return true;
            }
            break;
        }
        case kmldom::Type_LinearRing:
        {
            break;
        }
        case kmldom::Type_Polygon:
        {
            PolygonPtr poKmlPolygon = AsPolygon(poKmlGeometry);

            if (poKmlPolygon->has_altitudemode())
            {
                *pnAltitudeMode = poKmlPolygon->get_altitudemode();
                return true;
            }
            else if (poKmlPolygon->has_gx_altitudemode())
            {
                *pnAltitudeMode = poKmlPolygon->get_gx_altitudemode();
                *pbIsGX = true;
                return true;
            }

            break;
        }
        case kmldom::Type_MultiGeometry:
        {
            MultiGeometryPtr poKmlMultiGeometry =
                AsMultiGeometry(poKmlGeometry);

            const size_t nGeom = poKmlMultiGeometry->get_geometry_array_size();
            for (size_t i = 0; i < nGeom; i++)
            {
                if (kml2altitudemode_rec(
                        poKmlMultiGeometry->get_geometry_array_at(i),
                        pnAltitudeMode, pbIsGX))
                    return true;
            }

            break;
        }
        default:
        {
            break;
        }
    }

    return false;
}

/******************************************************************************
 Recursive function to read extrude from the geometry.
******************************************************************************/

static bool kml2extrude_rec(GeometryPtr poKmlGeometry, bool *pbExtrude)
{
    switch (poKmlGeometry->Type())
    {
        case kmldom::Type_Point:
        {
            PointPtr poKmlPoint = AsPoint(poKmlGeometry);

            if (poKmlPoint->has_extrude())
            {
                *pbExtrude = poKmlPoint->get_extrude();
                return true;
            }

            break;
        }
        case kmldom::Type_LineString:
        {
            LineStringPtr poKmlLineString = AsLineString(poKmlGeometry);

            if (poKmlLineString->has_extrude())
            {
                *pbExtrude = poKmlLineString->get_extrude();
                return true;
            }

            break;
        }
        case kmldom::Type_LinearRing:
        {
            break;
        }
        case kmldom::Type_Polygon:
        {
            PolygonPtr poKmlPolygon = AsPolygon(poKmlGeometry);

            if (poKmlPolygon->has_extrude())
            {
                *pbExtrude = poKmlPolygon->get_extrude();
                return true;
            }

            break;
        }
        case kmldom::Type_MultiGeometry:
        {
            MultiGeometryPtr poKmlMultiGeometry =
                AsMultiGeometry(poKmlGeometry);

            const size_t nGeom = poKmlMultiGeometry->get_geometry_array_size();
            for (size_t i = 0; i < nGeom; i++)
            {
                if (kml2extrude_rec(
                        poKmlMultiGeometry->get_geometry_array_at(i),
                        pbExtrude))
                    return true;
            }

            break;
        }
        default:
        {
            break;
        }
    }

    return false;
}

/******************************************************************************
 Recursive function to read tessellate from the geometry.
******************************************************************************/

static bool kml2tessellate_rec(GeometryPtr poKmlGeometry, int *pnTessellate)
{
    switch (poKmlGeometry->Type())
    {
        case kmldom::Type_Point:
        {
            break;
        }
        case kmldom::Type_LineString:
        {
            LineStringPtr poKmlLineString = AsLineString(poKmlGeometry);

            if (poKmlLineString->has_tessellate())
            {
                *pnTessellate = poKmlLineString->get_tessellate();
                return true;
            }

            break;
        }
        case kmldom::Type_LinearRing:
        {
            break;
        }
        case kmldom::Type_Polygon:
        {
            PolygonPtr poKmlPolygon = AsPolygon(poKmlGeometry);

            if (poKmlPolygon->has_tessellate())
            {
                *pnTessellate = poKmlPolygon->get_tessellate();
                return true;
            }

            break;
        }
        case kmldom::Type_MultiGeometry:
        {
            MultiGeometryPtr poKmlMultiGeometry =
                AsMultiGeometry(poKmlGeometry);

            const size_t nGeom = poKmlMultiGeometry->get_geometry_array_size();
            for (size_t i = 0; i < nGeom; i++)
            {
                if (kml2tessellate_rec(
                        poKmlMultiGeometry->get_geometry_array_at(i),
                        pnTessellate))
                    return true;
            }

            break;
        }
        default:
            break;
    }

    return false;
}

/************************************************************************/
/*                     ogrkmlSetAltitudeMode()                          */
/************************************************************************/

static void ogrkmlSetAltitudeMode(OGRFeature *poOgrFeat, int iField,
                                  int nAltitudeMode, bool bIsGX)
{
    if (!bIsGX)
    {
        switch (nAltitudeMode)
        {
            case kmldom::ALTITUDEMODE_CLAMPTOGROUND:
                poOgrFeat->SetField(iField, "clampToGround");
                break;

            case kmldom::ALTITUDEMODE_RELATIVETOGROUND:
                poOgrFeat->SetField(iField, "relativeToGround");
                break;

            case kmldom::ALTITUDEMODE_ABSOLUTE:
                poOgrFeat->SetField(iField, "absolute");
                break;
        }
    }
    else
    {
        switch (nAltitudeMode)
        {
            case kmldom::GX_ALTITUDEMODE_RELATIVETOSEAFLOOR:
                poOgrFeat->SetField(iField, "relativeToSeaFloor");
                break;

            case kmldom::GX_ALTITUDEMODE_CLAMPTOSEAFLOOR:
                poOgrFeat->SetField(iField, "clampToSeaFloor");
                break;
        }
    }
}

/************************************************************************/
/*                            TrimSpaces()                              */
/************************************************************************/

static const char *TrimSpaces(CPLString &oText)
{
    // SerializePretty() adds a new line before the data
    // ands trailing spaces. I believe this is wrong
    // as it breaks round-tripping.

    // Trim trailing spaces.
    while (!oText.empty() && oText.back() == ' ')
        oText.pop_back();

    // Skip leading newline and spaces.
    const char *pszText = oText.c_str();
    if (pszText[0] == '\n')
        pszText++;
    while (pszText[0] == ' ')
        pszText++;

    return pszText;
}

/************************************************************************/
/*                            kmldatetime2ogr()                         */
/************************************************************************/

static void kmldatetime2ogr(OGRFeature *poOgrFeat, const char *pszOGRField,
                            const std::string &osKmlDateTime)
{
    const int iField = poOgrFeat->GetFieldIndex(pszOGRField);

    if (iField > -1)
    {
        OGRField sField;

        if (OGRParseXMLDateTime(osKmlDateTime.c_str(), &sField))
            poOgrFeat->SetField(iField, &sField);
    }
}

/******************************************************************************
 function to read kml into ogr fields
******************************************************************************/

void kml2field(OGRFeature *poOgrFeat, FeaturePtr poKmlFeature,
               const fieldconfig &oFC)
{
    /***** id *****/

    if (poKmlFeature->has_id())
    {
        const std::string oKmlId = poKmlFeature->get_id();
        int iField = poOgrFeat->GetFieldIndex(oFC.idfield);

        if (iField > -1)
            poOgrFeat->SetField(iField, oKmlId.c_str());
    }
    /***** name *****/

    if (poKmlFeature->has_name())
    {
        const std::string oKmlName = poKmlFeature->get_name();
        int iField = poOgrFeat->GetFieldIndex(oFC.namefield);

        if (iField > -1)
            poOgrFeat->SetField(iField, oKmlName.c_str());
    }

    /***** description *****/

    if (poKmlFeature->has_description())
    {
        const std::string oKmlDesc = poKmlFeature->get_description();
        int iField = poOgrFeat->GetFieldIndex(oFC.descfield);

        if (iField > -1)
            poOgrFeat->SetField(iField, oKmlDesc.c_str());
    }

    if (poKmlFeature->has_timeprimitive())
    {
        TimePrimitivePtr poKmlTimePrimitive = poKmlFeature->get_timeprimitive();

        /***** timestamp *****/

        if (poKmlTimePrimitive->IsA(kmldom::Type_TimeStamp))
        {
            // Probably a libkml bug: AsTimeStamp should really return not NULL
            // on a gx:TimeStamp.
            TimeStampPtr poKmlTimeStamp = AsTimeStamp(poKmlTimePrimitive);
            if (!poKmlTimeStamp)
                poKmlTimeStamp = AsGxTimeStamp(poKmlTimePrimitive);

            if (poKmlTimeStamp && poKmlTimeStamp->has_when())
            {
                const std::string oKmlWhen = poKmlTimeStamp->get_when();
                kmldatetime2ogr(poOgrFeat, oFC.tsfield, oKmlWhen);
            }
        }

        /***** timespan *****/

        if (poKmlTimePrimitive->IsA(kmldom::Type_TimeSpan))
        {
            // Probably a libkml bug: AsTimeSpan should really return not NULL
            // on a gx:TimeSpan.
            TimeSpanPtr poKmlTimeSpan = AsTimeSpan(poKmlTimePrimitive);
            if (!poKmlTimeSpan)
                poKmlTimeSpan = AsGxTimeSpan(poKmlTimePrimitive);

            /***** begin *****/

            if (poKmlTimeSpan && poKmlTimeSpan->has_begin())
            {
                const std::string oKmlWhen = poKmlTimeSpan->get_begin();
                kmldatetime2ogr(poOgrFeat, oFC.beginfield, oKmlWhen);
            }

            /***** end *****/

            if (poKmlTimeSpan && poKmlTimeSpan->has_end())
            {
                const std::string oKmlWhen = poKmlTimeSpan->get_end();
                kmldatetime2ogr(poOgrFeat, oFC.endfield, oKmlWhen);
            }
        }
    }

    /***** placemark *****/
    PlacemarkPtr poKmlPlacemark = AsPlacemark(poKmlFeature);
    GroundOverlayPtr poKmlGroundOverlay = AsGroundOverlay(poKmlFeature);
    if (poKmlPlacemark && poKmlPlacemark->has_geometry())
    {
        GeometryPtr poKmlGeometry = poKmlPlacemark->get_geometry();

        /***** altitudeMode *****/
        int bIsGX = false;
        int nAltitudeMode = -1;

        int iField = poOgrFeat->GetFieldIndex(oFC.altitudeModefield);

        if (iField > -1)
        {
            if (kml2altitudemode_rec(poKmlGeometry, &nAltitudeMode, &bIsGX))
            {
                ogrkmlSetAltitudeMode(poOgrFeat, iField, nAltitudeMode,
                                      CPL_TO_BOOL(bIsGX));
            }
        }

        /***** tessellate *****/
        int nTessellate = -1;

        kml2tessellate_rec(poKmlGeometry, &nTessellate);

        iField = poOgrFeat->GetFieldIndex(oFC.tessellatefield);
        if (iField > -1)
            poOgrFeat->SetField(iField, nTessellate);

        /***** extrude *****/

        bool bExtrude = false;

        kml2extrude_rec(poKmlGeometry, &bExtrude);

        iField = poOgrFeat->GetFieldIndex(oFC.extrudefield);
        if (iField > -1)
            poOgrFeat->SetField(iField, bExtrude ? 1 : 0);

        /***** special case for gx:Track ******/
        /* we set the first timestamp as begin and the last one as end */
        if (poKmlGeometry->Type() == kmldom::Type_GxTrack &&
            !poKmlFeature->has_timeprimitive())
        {
            GxTrackPtr poKmlGxTrack = AsGxTrack(poKmlGeometry);
            if (poKmlGxTrack)
            {
                const size_t nCoords = poKmlGxTrack->get_when_array_size();
                if (nCoords > 0)
                {
                    kmldatetime2ogr(poOgrFeat, oFC.beginfield,
                                    poKmlGxTrack->get_when_array_at(0).c_str());
                    kmldatetime2ogr(
                        poOgrFeat, oFC.endfield,
                        poKmlGxTrack->get_when_array_at(nCoords - 1).c_str());
                }
            }
        }

        /***** special case for gx:MultiTrack ******/
        /* we set the first timestamp as begin and the last one as end */
        else if (poKmlGeometry->Type() == kmldom::Type_GxMultiTrack &&
                 !poKmlFeature->has_timeprimitive())
        {
            GxMultiTrackPtr poKmlGxMultiTrack = AsGxMultiTrack(poKmlGeometry);
            if (poKmlGxMultiTrack)
            {
                const size_t nGeom =
                    poKmlGxMultiTrack->get_gx_track_array_size();
                if (nGeom >= 1)
                {
                    {
                        GxTrackPtr poKmlGxTrack =
                            poKmlGxMultiTrack->get_gx_track_array_at(0);
                        const size_t nCoords =
                            poKmlGxTrack->get_when_array_size();
                        if (nCoords > 0)
                        {
                            kmldatetime2ogr(
                                poOgrFeat, oFC.beginfield,
                                poKmlGxTrack->get_when_array_at(0).c_str());
                        }
                    }

                    {
                        GxTrackPtr poKmlGxTrack =
                            poKmlGxMultiTrack->get_gx_track_array_at(nGeom - 1);
                        const size_t nCoords =
                            poKmlGxTrack->get_when_array_size();
                        if (nCoords > 0)
                        {
                            kmldatetime2ogr(
                                poOgrFeat, oFC.endfield,
                                poKmlGxTrack->get_when_array_at(nCoords - 1)
                                    .c_str());
                        }
                    }
                }
            }
        }
    }

    /***** camera *****/

    else if (poKmlPlacemark && poKmlPlacemark->has_abstractview() &&
             poKmlPlacemark->get_abstractview()->IsA(kmldom::Type_Camera))
    {
        const CameraPtr &camera = AsCamera(poKmlPlacemark->get_abstractview());

        if (camera->has_heading())
        {
            int iField = poOgrFeat->GetFieldIndex(oFC.headingfield);
            if (iField > -1)
                poOgrFeat->SetField(iField, camera->get_heading());
        }

        if (camera->has_tilt())
        {
            int iField = poOgrFeat->GetFieldIndex(oFC.tiltfield);
            if (iField > -1)
                poOgrFeat->SetField(iField, camera->get_tilt());
        }

        if (camera->has_roll())
        {
            int iField = poOgrFeat->GetFieldIndex(oFC.rollfield);
            if (iField > -1)
                poOgrFeat->SetField(iField, camera->get_roll());
        }

        int iField = poOgrFeat->GetFieldIndex(oFC.altitudeModefield);

        if (iField > -1)
        {
            if (camera->has_altitudemode())
            {
                const int nAltitudeMode = camera->get_altitudemode();
                ogrkmlSetAltitudeMode(poOgrFeat, iField, nAltitudeMode, false);
            }
            else if (camera->has_gx_altitudemode())
            {
                const int nAltitudeMode = camera->get_gx_altitudemode();
                ogrkmlSetAltitudeMode(poOgrFeat, iField, nAltitudeMode, true);
            }
        }
    }
    /***** ground overlay *****/
    else if (poKmlGroundOverlay)
    {
        /***** icon *****/
        int iField = poOgrFeat->GetFieldIndex(oFC.iconfield);
        if (iField > -1)
        {
            if (poKmlGroundOverlay->has_icon())
            {
                IconPtr icon = poKmlGroundOverlay->get_icon();
                if (icon->has_href())
                {
                    poOgrFeat->SetField(iField, icon->get_href().c_str());
                }
            }
        }

        /***** drawOrder *****/
        iField = poOgrFeat->GetFieldIndex(oFC.drawOrderfield);
        if (iField > -1)
        {
            if (poKmlGroundOverlay->has_draworder())
            {
                poOgrFeat->SetField(iField,
                                    poKmlGroundOverlay->get_draworder());
            }
        }

        /***** altitudeMode *****/

        iField = poOgrFeat->GetFieldIndex(oFC.altitudeModefield);

        if (iField > -1)
        {
            if (poKmlGroundOverlay->has_altitudemode())
            {
                switch (poKmlGroundOverlay->get_altitudemode())
                {
                    case kmldom::ALTITUDEMODE_CLAMPTOGROUND:
                        poOgrFeat->SetField(iField, "clampToGround");
                        break;

                    case kmldom::ALTITUDEMODE_RELATIVETOGROUND:
                        poOgrFeat->SetField(iField, "relativeToGround");
                        break;

                    case kmldom::ALTITUDEMODE_ABSOLUTE:
                        poOgrFeat->SetField(iField, "absolute");
                        break;
                }
            }
            else if (poKmlGroundOverlay->has_gx_altitudemode())
            {
                switch (poKmlGroundOverlay->get_gx_altitudemode())
                {
                    case kmldom::GX_ALTITUDEMODE_RELATIVETOSEAFLOOR:
                        poOgrFeat->SetField(iField, "relativeToSeaFloor");
                        break;

                    case kmldom::GX_ALTITUDEMODE_CLAMPTOSEAFLOOR:
                        poOgrFeat->SetField(iField, "clampToSeaFloor");
                        break;
                }
            }
        }
    }

    /***** visibility *****/
    const int nVisibility =
        poKmlFeature->has_visibility() ? poKmlFeature->get_visibility() : -1;

    int iField = poOgrFeat->GetFieldIndex(oFC.visibilityfield);

    if (iField > -1)
        poOgrFeat->SetField(iField, nVisibility);

    /***** snippet *****/

    if (poKmlFeature->has_snippet())
    {
        CPLString oText = poKmlFeature->get_snippet()->get_text();

        iField = poOgrFeat->GetFieldIndex(oFC.snippetfield);

        if (iField > -1)
            poOgrFeat->SetField(iField, TrimSpaces(oText));
    }

    /***** extended schema *****/
    ExtendedDataPtr poKmlExtendedData = nullptr;

    if (poKmlFeature->has_extendeddata())
    {
        poKmlExtendedData = poKmlFeature->get_extendeddata();

        /***** loop over the schemadata_arrays *****/

        const size_t nSchemaData =
            poKmlExtendedData->get_schemadata_array_size();

        for (size_t iSchemaData = 0; iSchemaData < nSchemaData; iSchemaData++)
        {
            SchemaDataPtr poKmlSchemaData =
                poKmlExtendedData->get_schemadata_array_at(iSchemaData);

            /***** loop over the simpledata array *****/

            const size_t nSimpleData =
                poKmlSchemaData->get_simpledata_array_size();

            for (size_t iSimpleData = 0; iSimpleData < nSimpleData;
                 iSimpleData++)
            {
                SimpleDataPtr poKmlSimpleData =
                    poKmlSchemaData->get_simpledata_array_at(iSimpleData);

                /***** find the field index *****/

                iField = -1;

                if (poKmlSimpleData->has_name())
                {
                    const string oName = poKmlSimpleData->get_name();
                    const char *pszName = oName.c_str();

                    iField = poOgrFeat->GetFieldIndex(pszName);
                }

                /***** if it has trxt set the field *****/

                if (iField > -1 && poKmlSimpleData->has_text())
                {
                    CPLString oText = poKmlSimpleData->get_text();

                    poOgrFeat->SetField(iField, TrimSpaces(oText));
                }
            }
        }

        if (nSchemaData == 0 && poKmlExtendedData->get_data_array_size() > 0)
        {
            const bool bLaunderFieldNames = CPLTestBool(
                CPLGetConfigOption("LIBKML_LAUNDER_FIELD_NAMES", "YES"));
            const size_t nDataArraySize =
                poKmlExtendedData->get_data_array_size();
            for (size_t i = 0; i < nDataArraySize; i++)
            {
                const DataPtr &data = poKmlExtendedData->get_data_array_at(i);
                if (data->has_name() && data->has_value())
                {
                    CPLString osName = std::string(data->get_name());
                    if (bLaunderFieldNames)
                        osName = OGRLIBKMLLayer::LaunderFieldNames(osName);
                    iField = poOgrFeat->GetFieldIndex(osName);
                    if (iField >= 0)
                    {
                        poOgrFeat->SetField(iField, data->get_value().c_str());
                    }
                }
            }
        }
    }
}

/******************************************************************************
 function create a simplefield from a FieldDefn
******************************************************************************/

SimpleFieldPtr FieldDef2kml(const OGRFieldDefn *poOgrFieldDef,
                            KmlFactory *poKmlFactory, bool bApproxOK,
                            const fieldconfig &oFC)
{
    const char *pszFieldName = poOgrFieldDef->GetNameRef();

    if (EQUAL(pszFieldName, oFC.idfield) ||
        EQUAL(pszFieldName, oFC.namefield) ||
        EQUAL(pszFieldName, oFC.descfield) ||
        EQUAL(pszFieldName, oFC.tsfield) ||
        EQUAL(pszFieldName, oFC.beginfield) ||
        EQUAL(pszFieldName, oFC.endfield) ||
        EQUAL(pszFieldName, oFC.altitudeModefield) ||
        EQUAL(pszFieldName, oFC.tessellatefield) ||
        EQUAL(pszFieldName, oFC.extrudefield) ||
        EQUAL(pszFieldName, oFC.visibilityfield) ||
        EQUAL(pszFieldName, oFC.drawOrderfield) ||
        EQUAL(pszFieldName, oFC.iconfield) ||
        EQUAL(pszFieldName, oFC.headingfield) ||
        EQUAL(pszFieldName, oFC.tiltfield) ||
        EQUAL(pszFieldName, oFC.rollfield) ||
        EQUAL(pszFieldName, oFC.snippetfield) ||
        EQUAL(pszFieldName, oFC.modelfield) ||
        EQUAL(pszFieldName, oFC.scalexfield) ||
        EQUAL(pszFieldName, oFC.scaleyfield) ||
        EQUAL(pszFieldName, oFC.scalezfield) ||
        EQUAL(pszFieldName, oFC.networklinkfield) ||
        EQUAL(pszFieldName, oFC.networklink_refreshvisibility_field) ||
        EQUAL(pszFieldName, oFC.networklink_flytoview_field) ||
        EQUAL(pszFieldName, oFC.networklink_refreshMode_field) ||
        EQUAL(pszFieldName, oFC.networklink_refreshInterval_field) ||
        EQUAL(pszFieldName, oFC.networklink_viewRefreshMode_field) ||
        EQUAL(pszFieldName, oFC.networklink_viewRefreshTime_field) ||
        EQUAL(pszFieldName, oFC.networklink_viewBoundScale_field) ||
        EQUAL(pszFieldName, oFC.networklink_viewFormat_field) ||
        EQUAL(pszFieldName, oFC.networklink_httpQuery_field) ||
        EQUAL(pszFieldName, oFC.camera_longitude_field) ||
        EQUAL(pszFieldName, oFC.camera_latitude_field) ||
        EQUAL(pszFieldName, oFC.camera_altitude_field) ||
        EQUAL(pszFieldName, oFC.camera_altitudemode_field) ||
        EQUAL(pszFieldName, oFC.photooverlayfield) ||
        EQUAL(pszFieldName, oFC.leftfovfield) ||
        EQUAL(pszFieldName, oFC.rightfovfield) ||
        EQUAL(pszFieldName, oFC.bottomfovfield) ||
        EQUAL(pszFieldName, oFC.topfovfield) ||
        EQUAL(pszFieldName, oFC.nearfield) ||
        EQUAL(pszFieldName, oFC.photooverlay_shape_field) ||
        EQUAL(pszFieldName, oFC.imagepyramid_tilesize_field) ||
        EQUAL(pszFieldName, oFC.imagepyramid_maxwidth_field) ||
        EQUAL(pszFieldName, oFC.imagepyramid_maxheight_field) ||
        EQUAL(pszFieldName, oFC.imagepyramid_gridorigin_field))
    {
        return nullptr;
    }

    SimpleFieldPtr poKmlSimpleField = poKmlFactory->CreateSimpleField();
    poKmlSimpleField->set_name(pszFieldName);

    switch (poOgrFieldDef->GetType())
    {
        case OFTInteger:
        case OFTIntegerList:
            poKmlSimpleField->set_type(
                poOgrFieldDef->GetSubType() == OFSTBoolean ? "bool" : "int");
            return poKmlSimpleField;

        case OFTReal:
        case OFTRealList:
            poKmlSimpleField->set_type(
                poOgrFieldDef->GetSubType() == OFSTFloat32 ? "float"
                                                           : "double");
            return poKmlSimpleField;

        case OFTString:
        case OFTStringList:
            poKmlSimpleField->set_type("string");
            return poKmlSimpleField;

        case OFTInteger64:
            if (bApproxOK)
            {
                poKmlSimpleField->set_type("string");
                return poKmlSimpleField;
            }
            break;

            /***** kml has these types but as timestamp/timespan *****/
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
            if (bApproxOK)
            {
                poKmlSimpleField->set_type("string");
                return poKmlSimpleField;
            }
            break;

        default:
            poKmlSimpleField->set_type("string");
            return poKmlSimpleField;
    }

    return nullptr;
}

/******************************************************************************
 function to add the simpleFields in a schema to a featuredefn
******************************************************************************/

void kml2FeatureDef(SchemaPtr poKmlSchema, OGRFeatureDefn *poOgrFeatureDefn)
{
    const size_t nSimpleFields = poKmlSchema->get_simplefield_array_size();

    for (size_t iSimpleField = 0; iSimpleField < nSimpleFields; iSimpleField++)
    {
        SimpleFieldPtr poKmlSimpleField =
            poKmlSchema->get_simplefield_array_at(iSimpleField);

        const char *pszType = "string";
        string osName = "Unknown";
        string osType;

        if (poKmlSimpleField->has_type())
        {
            osType = poKmlSimpleField->get_type();

            pszType = osType.c_str();
        }

        // TODO: We cannot set displayname as the field name because in
        // kml2field() we make the lookup on fields based on their name. We
        // would need some map if we really want to use displayname, but that
        // might not be a good idea because displayname may have HTML
        // formatting, which makes it impractical when converting to other
        // drivers or to make requests.
        // Example: http://www.jasonbirch.com/files/newt_combined.kml

        // if( poKmlSimpleField->has_displayname() )
        //   {
        //       osName = poKmlSimpleField->get_displayname();
        //   }
        //   else
        if (poKmlSimpleField->has_name())
        {
            osName = poKmlSimpleField->get_name();
        }
        if (poOgrFeatureDefn->GetFieldIndex(osName.c_str()) < 0)
        {
            if (EQUAL(pszType, "bool") || EQUAL(pszType, "boolean"))
            {
                OGRFieldDefn ogrFieldDefn(osName.c_str(), OFTInteger);
                ogrFieldDefn.SetSubType(OFSTBoolean);
                poOgrFeatureDefn->AddFieldDefn(&ogrFieldDefn);
            }
            else if (EQUAL(pszType, "int") || EQUAL(pszType, "short") ||
                     EQUAL(pszType, "ushort"))
            {
                OGRFieldDefn ogrFieldDefn(osName.c_str(), OFTInteger);
                poOgrFeatureDefn->AddFieldDefn(&ogrFieldDefn);
            }
            else if (EQUAL(pszType, "uint"))
            {
                OGRFieldDefn ogrFieldDefn(osName.c_str(), OFTInteger64);
                poOgrFeatureDefn->AddFieldDefn(&ogrFieldDefn);
            }
            else if (EQUAL(pszType, "float") || EQUAL(pszType, "double"))
            {
                // We write correctly 'double' for 64-bit since GDAL 3.11.1.
                // In prior versions we wrote 'float', so it is premature
                // on reading to set OFSTFloat32 when reading 'float'
                OGRFieldDefn ogrFieldDefn(osName.c_str(), OFTReal);
                poOgrFeatureDefn->AddFieldDefn(&ogrFieldDefn);
            }
            else  // string, or any other unrecognized type.
            {
                OGRFieldDefn ogrFieldDefn(osName.c_str(), OFTString);
                poOgrFeatureDefn->AddFieldDefn(&ogrFieldDefn);
            }
        }
    }
}

/*******************************************************************************
 * function to fetch the field config options
 *
 *******************************************************************************/

void get_fieldconfig(struct fieldconfig *oFC)
{
    oFC->idfield = CPLGetConfigOption("LIBKML_ID_FIELD", "id");
    oFC->namefield = CPLGetConfigOption("LIBKML_NAME_FIELD", "Name");
    oFC->descfield =
        CPLGetConfigOption("LIBKML_DESCRIPTION_FIELD", "description");
    oFC->tsfield = CPLGetConfigOption("LIBKML_TIMESTAMP_FIELD", "timestamp");
    oFC->beginfield = CPLGetConfigOption("LIBKML_BEGIN_FIELD", "begin");
    oFC->endfield = CPLGetConfigOption("LIBKML_END_FIELD", "end");
    oFC->altitudeModefield =
        CPLGetConfigOption("LIBKML_ALTITUDEMODE_FIELD", "altitudeMode");
    oFC->tessellatefield =
        CPLGetConfigOption("LIBKML_TESSELLATE_FIELD", "tessellate");
    oFC->extrudefield = CPLGetConfigOption("LIBKML_EXTRUDE_FIELD", "extrude");
    oFC->visibilityfield =
        CPLGetConfigOption("LIBKML_VISIBILITY_FIELD", "visibility");
    oFC->drawOrderfield =
        CPLGetConfigOption("LIBKML_DRAWORDER_FIELD", "drawOrder");
    oFC->iconfield = CPLGetConfigOption("LIBKML_ICON_FIELD", "icon");
    oFC->headingfield = CPLGetConfigOption("LIBKML_HEADING_FIELD", "heading");
    oFC->tiltfield = CPLGetConfigOption("LIBKML_TILT_FIELD", "tilt");
    oFC->rollfield = CPLGetConfigOption("LIBKML_ROLL_FIELD", "roll");
    oFC->snippetfield = CPLGetConfigOption("LIBKML_SNIPPET_FIELD", "snippet");
    oFC->modelfield = CPLGetConfigOption("LIBKML_MODEL_FIELD", "model");
    oFC->scalexfield = CPLGetConfigOption("LIBKML_SCALE_X_FIELD", "scale_x");
    oFC->scaleyfield = CPLGetConfigOption("LIBKML_SCALE_Y_FIELD", "scale_y");
    oFC->scalezfield = CPLGetConfigOption("LIBKML_SCALE_Z_FIELD", "scale_z");
    oFC->networklinkfield =
        CPLGetConfigOption("LIBKML_NETWORKLINK_FIELD", "networklink");
    oFC->networklink_refreshvisibility_field =
        CPLGetConfigOption("LIBKML_NETWORKLINK_REFRESHVISIBILITY_FIELD",
                           "networklink_refreshvisibility");
    oFC->networklink_flytoview_field = CPLGetConfigOption(
        "LIBKML_NETWORKLINK_FLYTOVIEW_FIELD", "networklink_flytoview");
    oFC->networklink_refreshMode_field = CPLGetConfigOption(
        "LIBKML_NETWORKLINK_REFRESHMODE_FIELD", "networklink_refreshmode");
    oFC->networklink_refreshInterval_field =
        CPLGetConfigOption("LIBKML_NETWORKLINK_REFRESHINTERVAL_FIELD",
                           "networklink_refreshinterval");
    oFC->networklink_viewRefreshMode_field =
        CPLGetConfigOption("LIBKML_NETWORKLINK_VIEWREFRESHMODE_FIELD",
                           "networklink_viewrefreshmode");
    oFC->networklink_viewRefreshTime_field =
        CPLGetConfigOption("LIBKML_NETWORKLINK_VIEWREFRESHTIME_FIELD",
                           "networklink_viewrefreshtime");
    oFC->networklink_viewBoundScale_field =
        CPLGetConfigOption("LIBKML_NETWORKLINK_VIEWBOUNDSCALE_FIELD",
                           "networklink_viewboundscale");
    oFC->networklink_viewFormat_field = CPLGetConfigOption(
        "LIBKML_NETWORKLINK_VIEWFORMAT_FIELD", "networklink_viewformat");
    oFC->networklink_httpQuery_field = CPLGetConfigOption(
        "LIBKML_NETWORKLINK_HTTPQUERY_FIELD", "networklink_httpquery");
    oFC->camera_longitude_field =
        CPLGetConfigOption("LIBKML_CAMERA_LONGITUDE_FIELD", "camera_longitude");
    oFC->camera_latitude_field =
        CPLGetConfigOption("LIBKML_CAMERA_LATITUDE_FIELD", "camera_latitude");
    oFC->camera_altitude_field =
        CPLGetConfigOption("LIBKML_CAMERA_ALTITUDE_FIELD", "camera_altitude");
    oFC->camera_altitudemode_field = CPLGetConfigOption(
        "LIBKML_CAMERA_ALTITUDEMODE_FIELD", "camera_altitudemode");
    oFC->photooverlayfield =
        CPLGetConfigOption("LIBKML_PHOTOOVERLAY_FIELD", "photooverlay");
    oFC->leftfovfield = CPLGetConfigOption("LIBKML_LEFTFOV_FIELD", "leftfov");
    oFC->rightfovfield =
        CPLGetConfigOption("LIBKML_RIGHTFOV_FIELD", "rightfov");
    oFC->bottomfovfield =
        CPLGetConfigOption("LIBKML_BOTTOMFOV_FIELD", "bottomfov");
    oFC->topfovfield = CPLGetConfigOption("LIBKML_TOPFOV_FIELD", "topfov");
    oFC->nearfield = CPLGetConfigOption("LIBKML_NEARFOV_FIELD", "near");
    oFC->photooverlay_shape_field = CPLGetConfigOption(
        "LIBKML_PHOTOOVERLAY_SHAPE_FIELD", "photooverlay_shape");
    oFC->imagepyramid_tilesize_field = CPLGetConfigOption(
        "LIBKML_IMAGEPYRAMID_TILESIZE", "imagepyramid_tilesize");
    oFC->imagepyramid_maxwidth_field = CPLGetConfigOption(
        "LIBKML_IMAGEPYRAMID_MAXWIDTH", "imagepyramid_maxwidth");
    oFC->imagepyramid_maxheight_field = CPLGetConfigOption(
        "LIBKML_IMAGEPYRAMID_MAXHEIGHT", "imagepyramid_maxheight");
    oFC->imagepyramid_gridorigin_field = CPLGetConfigOption(
        "LIBKML_IMAGEPYRAMID_GRIDORIGIN", "imagepyramid_gridorigin");
}

/************************************************************************/
/*                 kmlAltitudeModeFromString()                          */
/************************************************************************/

int kmlAltitudeModeFromString(const char *pszAltitudeMode, int &isGX)
{
    isGX = FALSE;
    int iAltitudeMode = static_cast<int>(kmldom::ALTITUDEMODE_CLAMPTOGROUND);

    if (EQUAL(pszAltitudeMode, "clampToGround"))
    {
        iAltitudeMode = static_cast<int>(kmldom::ALTITUDEMODE_CLAMPTOGROUND);
    }
    else if (EQUAL(pszAltitudeMode, "relativeToGround"))
    {
        iAltitudeMode = static_cast<int>(kmldom::ALTITUDEMODE_RELATIVETOGROUND);
    }
    else if (EQUAL(pszAltitudeMode, "absolute"))
    {
        iAltitudeMode = static_cast<int>(kmldom::ALTITUDEMODE_ABSOLUTE);
    }
    else if (EQUAL(pszAltitudeMode, "relativeToSeaFloor"))
    {
        iAltitudeMode =
            static_cast<int>(kmldom::GX_ALTITUDEMODE_RELATIVETOSEAFLOOR);
        isGX = TRUE;
    }
    else if (EQUAL(pszAltitudeMode, "clampToSeaFloor"))
    {
        iAltitudeMode =
            static_cast<int>(kmldom::GX_ALTITUDEMODE_CLAMPTOSEAFLOOR);
        isGX = TRUE;
    }
    else
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Unrecognized value for altitudeMode: %s", pszAltitudeMode);
    }

    return iAltitudeMode;
}
