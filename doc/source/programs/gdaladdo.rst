.. _gdaladdo:

================================================================================
gdaladdo
================================================================================

.. only:: html

    Builds or rebuilds overview images.

.. Index:: gdaladdo

Synopsis
--------

.. program-output:: gdaladdo --help-doc

Description
-----------

The :program:`gdaladdo` utility can be used to build or rebuild overview images for
most supported file formats with one of several downsampling algorithms.

.. program:: gdaladdo

.. include:: options/help_and_help_general.rst

.. option:: -r {nearest|average|rms|gauss|bilinear|cubic|cubicspline|lanczos|average_magphase|mode}

    Select a resampling algorithm. The default is ``nearest``, which is generally not
    appropriate if sub-pixel accuracy is desired.

    Starting with GDAL 3.9, when refreshing existing TIFF overviews, the previously
    used method, as noted in the RESAMPLING metadata item of the overview, will
    be used if :option:`-r` is not specified.

    The available methods are:

    ``nearest`` applies a nearest neighbour (simple sampling) resampler.

    ``average`` computes the average of all non-NODATA contributing pixels. Starting with GDAL 3.1, this is a weighted average taking into account properly the weight of source pixels not contributing fully to the target pixel.

    ``rms`` computes the root mean squared / quadratic mean of all non-NODATA contributing pixels (GDAL >= 3.3)

    ``gauss`` applies a Gaussian kernel before computing the overview,
    which can lead to better results than simple averaging in e.g case of sharp edges
    with high contrast or noisy patterns. The advised level values should be 2, 4, 8, ...
    so that a 3x3 resampling Gaussian kernel is selected.

    ``bilinear`` applies a bilinear convolution kernel.

    ``cubic`` applies a cubic convolution kernel.

    ``cubicspline`` applies a B-Spline convolution kernel.

    ``lanczos`` applies a Lanczos windowed sinc convolution kernel.

    ``average_magphase`` averages complex data in mag/phase space.

    ``mode`` selects the value which appears most often of all the sampled points.

.. option:: -b <band>

    Select an input band **band** for overview generation. Band numbering
    starts from 1. Multiple :option:`-b` switches may be used to select a set
    of input bands to generate overviews.

.. option:: -ro

    open the dataset in read-only mode, in order to generate external overview
    (for GeoTIFF especially).

.. option:: -clean

    remove all overviews.

.. option:: -oo <NAME>=<VALUE>

    Dataset open option (format specific)

.. option:: -minsize <val>

    Maximum width or height of the smallest overview level. Only taken into
    account if explicit levels are not specified. Defaults to 256.

    .. versionadded:: 2.3

.. option:: --partial-refresh-from-source-timestamp

    .. versionadded:: 3.8

    This option performs a partial refresh of existing overviews, when <filename>
    is a VRT file with an external overview.
    It checks the modification timestamp of all the sources of the VRT
    and regenerate the overview for areas corresponding to sources whose
    timestamp is more recent than the external overview of the VRT.
    By default all existing overview levels will be refreshed, unless explicit
    levels are specified. See :example:`refresh-vrt`.

.. option:: --partial-refresh-from-projwin <ulx> <uly> <lrx> <lry>

    .. versionadded:: 3.8

    This option performs a partial refresh of existing overviews, in the region
    of interest specified by georeference coordinates where <ulx> is the X value
    of the upper left corner, <uly> is the Y value of the upper left corner,
    <lrx> is the X value of the lower right corner and <lry> is the Y value of
    the lower right corner.
    By default all existing overview levels will be refreshed, unless explicit
    levels are specified.

.. option:: --partial-refresh-from-source-extent <filename1>[,<filenameN>]...

    .. versionadded:: 3.8

    This option performs a partial refresh of existing overviews, in the region
    of interest specified by one or several filenames (names separated by comma).
    Note that the filenames are only used to determine the regions of interest
    to refresh. The reference source pixels are the one of the main dataset.
    By default all existing overview levels will be refreshed, unless explicit
    levels are specified. See :example:`refresh-tiff`.

.. option:: <filename>

    The file to build overviews for (or whose overviews must be removed).

.. option:: <levels>

    A list of integral overview levels to build. Ignored with :option:`-clean` option.

    .. versionadded:: 2.3

        Levels are no longer required to build overviews.
        In which case, appropriate overview power-of-two factors will be selected
        until the smallest overview is smaller than the value of the -minsize switch.

        Starting with GDAL 3.9, if there are already existing overviews, the
        corresponding levels will be used to refresh them if no explicit levels
        are specified.


gdaladdo will honour properly NODATA_VALUES tuples (special dataset metadata) so
that only a given RGB triplet (in case of a RGB image) will be considered as the
nodata value and not each value of the triplet independently per band.

Selecting a level value like ``2`` causes an overview level that is 1/2
the resolution (in each dimension) of the base layer to be computed.  If
the file has existing overview levels at a level selected, those levels will
be recomputed and rewritten in place.

For internal GeoTIFF overviews (or external overviews in GeoTIFF format), note
that -clean does not shrink the file. A later run of gdaladdo with overview levels
will cause the file to be expanded, rather than reusing the space of the previously
deleted overviews. If you just want to change the resampling method on a file that
already has overviews computed, you don't need to clean the existing overviews.

Some format drivers do not support overviews at all.  Many format drivers
store overviews in a secondary file with the extension .ovr that is actually
in TIFF format.  By default, the GeoTIFF driver stores overviews internally to the file
operated on (if it is writable), unless the -ro flag is specified.

Most drivers also support an alternate overview format using Erdas Imagine
format.  To trigger this use the :config:`USE_RRD=YES` configuration option (:example:`use-rrd`).  This will
place the overviews in an associated .aux file suitable for direct use with
Imagine or ArcGIS as well as GDAL applications.  (e.g. --config USE_RRD YES)

External overviews in GeoTIFF format
------------------------------------

External overviews created in TIFF format may be compressed using the :config:`COMPRESS_OVERVIEW`
configuration option.  All compression methods, supported by the GeoTIFF
driver, are available here. (e.g. ``--config COMPRESS_OVERVIEW DEFLATE``).
The photometric interpretation can be set with the :config:`PHOTOMETRIC_OVERVIEW`
=RGB/YCBCR/... configuration option,
and the interleaving with the :config:`INTERLEAVE_OVERVIEW` =PIXEL/BAND configuration option.

Since GDAL 3.6, :config:`COMPRESS_OVERVIEW` and :config:`INTERLEAVE_OVERVIEW`
are honoured when creating internal overviews of TIFF files.

For JPEG compressed external and internal overviews, the JPEG quality can be set with
``--config JPEG_QUALITY_OVERVIEW value``.

For WEBP compressed external and internal overviews, the WEBP quality level can be set with
``--config WEBP_LEVEL_OVERVIEW value``. If not set, will default to 75.

For WEBP compressed external and internal overviews, the WEBP lossless/lossy switch can be set with
``--config WEBP_LOSSLESS_OVERVIEW value``. If not set, will default to NO (lossy). Added in GDAL 3.6.0

For LERC compressed external and internal overviews, the max error threshold can be set with
``--config MAX_Z_ERROR_OVERVIEW value``. If not set, will default to 0 (lossless). Added in GDAL 3.4.1

For DEFLATE or LERC_DEFLATE compressed external and internal overviews, the compression level can be set with
``--config ZLEVEL_OVERVIEW value``. If not set, will default to 6. Added in GDAL 3.4.1

For ZSTD or LERC_ZSTD compressed external and internal overviews, the compression level can be set with
``--config ZSTD_LEVEL_OVERVIEW value``. If not set, will default to 9. Added in GDAL 3.4.1

For JPEG-XL compressed external and internal overviews, the following settings can be set since GDAL 3.9.0:

* Whether compression should be lossless with ``--config JXL_LOSSLESS_OVERVIEW YES|NO``. Default is YES

* Level of effort with ``--config JXL_EFFORT_OVERVIEW value``, with value between 1(fast) and 9(flow). Default is 5

* Distance level for lossy compression with ``--config JXL_DISTANCE_OVERVIEW value``, with value: 0=mathematically lossless, 1.0=visually lossless, usual range [0.5,3]. Default is 1.0. Ignored if JXL_LOSSLESS_OVERVIEW is YES

* Distance level for lossy compression of alpha channel with ``--config JXL_ALPHA_DISTANCE_OVERVIEW value``, with value: 0=mathematically lossless, 1.0=visually lossless, usual range [0.5,3]. Default is the same value as JXL_DISTANCE_OVERVIEW. Ignored if JXL_LOSSLESS_OVERVIEW is YES


For LZW, ZSTD or DEFLATE compressed external overviews, the predictor value can be set
with ``--config PREDICTOR_OVERVIEW 1|2|3``.

To produce the smallest possible JPEG-In-TIFF overviews, you should use:

::

    --config COMPRESS_OVERVIEW JPEG --config PHOTOMETRIC_OVERVIEW YCBCR --config INTERLEAVE_OVERVIEW PIXEL

External overviews can be created in the BigTIFF format by using
the :config:`BIGTIFF_OVERVIEW` configuration option:
``--config BIGTIFF_OVERVIEW {IF_NEEDED|IF_SAFER|YES|NO}``.

The default value is IF_SAFER starting with GDAL 2.3.0 (previously was IF_NEEDED).
The behavior of this option is exactly the same as the BIGTIFF creation option
documented in the GeoTIFF driver documentation.

- YES forces BigTIFF.
- NO forces classic TIFF.
- IF_NEEDED will only create a BigTIFF if it is clearly needed (uncompressed,
  and overviews larger than 4GB).
- IF_SAFER will create BigTIFF if the resulting file *might* exceed 4GB.

Sparse GeoTIFF overview files (that is tiles which are omitted if all their pixels are
at the nodata value, when there's one, or at 0 otherwise) can be obtained with
``--config SPARSE_OK_OVERVIEW ON``. Added in GDAL 3.4.1

See the documentation of the :ref:`raster.gtiff` driver for further explanations on all those options.

Setting blocksize in Geotiff overviews
---------------------------------------

``--config GDAL_TIFF_OVR_BLOCKSIZE <size>``

Example: ``--config GDAL_TIFF_OVR_BLOCKSIZE 256``

Default value is 128, or starting with GDAL 3.1, if creating overviews on a tiled GeoTIFF file, the tile size of the full resolution image.
Note: without this setting, the file can have the full resolution image with a blocksize different from overviews blocksize.(e.g. full resolution image at blocksize 256, overviews at blocksize 128)


Nodata / source validity mask handling during resampling
--------------------------------------------------------

Invalid values in source pixels, either identified through a nodata value
metadata set on the source band, a mask band, an alpha band will not be used
during resampling.

.. include:: nodata_handling_gdaladdo_gdal_translate.rst

Multithreading
--------------

.. versionadded:: 3.2

The :config:`GDAL_NUM_THREADS` configuration option can be set to
``ALL_CPUS`` or a integer value to specify the number of threads to use for
overview computation.

C API
-----

Functionality of this utility can be done from C with :cpp:func:`GDALBuildOverviews`.

Examples
--------

.. example::
   :title: Create overviews, embedded in the supplied TIFF file, with automatic computation of levels

   .. code-block:: bash

       gdaladdo -r average abc.tif

.. example::
   :title: Create overviews, embedded in the supplied TIFF file

   .. code-block:: bash

       gdaladdo -r average abc.tif 2 4 8 16

.. example::
   :title: Create an external compressed GeoTIFF overview file from the ERDAS .IMG file

   .. code-block:: bash

       gdaladdo -ro --config COMPRESS_OVERVIEW DEFLATE erdas.img 2 4 8 16

.. example::
   :title: Create an external JPEG-compressed GeoTIFF overview file from a 3-band RGB dataset

   If the dataset is a writable GeoTIFF, you also need to add the :option:`-ro` option to
   force the generation of external overview.

   .. code-block:: bash

       gdaladdo --config COMPRESS_OVERVIEW JPEG --config PHOTOMETRIC_OVERVIEW YCBCR
                --config INTERLEAVE_OVERVIEW PIXEL rgb_dataset.ext 2 4 8 16

.. example::
   :title: Create Erdas Imagine format overviews for the indicated JPEG file
   :id: use-rrd

   .. code-block:: bash

       gdaladdo --config USE_RRD YES airphoto.jpg 3 9 27 81

.. example::
   :title: Create overviews for a specific subdataset

   For example, one of potentially many raster layers in a GeoPackage (the "filename" parameter must be driver prefix, filename and subdataset name, like e.g. shown by gdalinfo):

   .. code-block:: bash

       gdaladdo GPKG:file.gpkg:layer


.. example::
   :title: Refresh overviews of a VRT file
   :id: refresh-vrt

   This is needed when for sources have been modified after the .vrt.ovr generation:

   .. code-block:: bash

       gdalbuildvrt my.vrt tile1.tif tile2.tif                          # create VRT
       gdaladdo -r cubic my.vrt                                         # initial overview generation
       touch tile1.tif                                                  # simulate update of one of the source tiles
       gdaladdo --partial-refresh-from-source-timestamp -r cubic my.vrt # refresh overviews


.. example::
   :title: Refresh overviews of a TIFF file
   :id: refresh-tiff

   .. code-block:: bash

       gdalwarp -overwrite tile1.tif tile2.tif mosaic.tif                          # create mosaic
       gdaladdo -r cubic mosaic.tif                                                # initial overview generation
       touch tile1.tif                                                             # simulate update of one of the source tiles
       gdalwarp tile1.tif mosaic.tif                                               # update mosaic
       gdaladdo --partial-refresh-from-source-extent tile1.tif -r cubic mosaic.tif # refresh overviews
