/* X3F_DNGTAGS.C
 *
 * libtiff tag extender for DNG tags.
 *
 * The current version of libtiff (4.0.3) only supports tags for DNG
 * versions <= 1.1.0.0
 *
 * Copyright 2015 - Roland and Erik Karlsson
 * BSD-style - see doc/copyright.txt
 *
 */

#include "x3f_dngtags.h"

#include <stddef.h>
#include <tiffio.h>

static TIFFExtendProc previous_tag_extender = NULL;

static void tag_extender(TIFF *tiff)
{
  static const TIFFFieldInfo tags[] = {
    {TIFFTAG_BASELINEEXPOSURE, 1, 1, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 0,
     "BaselineExposure"},
    {TIFFTAG_CALIBRATIONILLUMINANT1, 1, 1, TIFF_SHORT, FIELD_CUSTOM, 1, 0,
     "CalibrationIlluminant1"},
    {TIFFTAG_EXTRACAMERAPROFILES, -1, -1, TIFF_LONG, FIELD_CUSTOM, 1, 1,
     "ExtraCameraProfiles"},
    {TIFFTAG_PROFILENAME, -1, -1, TIFF_ASCII, FIELD_CUSTOM, 1, 0,
     "ProfileName"},
    {TIFFTAG_ASSHOTPROFILENAME, -1, -1, TIFF_ASCII, FIELD_CUSTOM, 1, 0,
     "AsShotProfileName"},
    {TIFFTAG_OPCODELIST1, -1, -1, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1,
     "OpcodeList1"},
    {TIFFTAG_OPCODELIST2, -1, -1, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1,
     "OpcodeList2"},
    {TIFFTAG_OPCODELIST3, -1, -1, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1,
     "OpcodeList3"},
    {TIFFTAG_DEFAULTBLACKRENDER, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0,
     "DefaultBlackRender"},
    {TIFFTAG_FORWARDMATRIX1, -1, -1, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 1,
     "ForwardMatrix1"},
    {TIFFTAG_FORWARDMATRIX2, -1, -1, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 1,
     "ForwardMatrix2"},
    {TIFFTAG_PROFILETONECURVE, -1, -1, TIFF_FLOAT, FIELD_CUSTOM, 1, 1,
     "ProfileToneCurve"},
  };

  TIFFMergeFieldInfo(tiff, tags, sizeof(tags)/sizeof(tags[0]));

  if (previous_tag_extender)
    previous_tag_extender(tiff);
}

void x3f_install_dng_tags(void)
{
  static int installed = 0;
  if (!installed) {
    previous_tag_extender = TIFFSetTagExtender(tag_extender);
    installed = 1;
  }
}
