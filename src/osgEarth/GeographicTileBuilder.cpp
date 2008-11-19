#include <osgEarth/GeographicTileBuilder>
#include <osgEarth/Mercator>
#include <osgEarth/TerrainTileEdgeNormalizerUpdateCallback>
#include <osg/Image>
#include <osg/Notify>
#include <osg/PagedLOD>
#include <osg/CoordinateSystemNode>
#include <osg/Version>
#include <osgDB/ReadFile>
#include <osgTerrain/Terrain>
#include <osgTerrain/TerrainTile>
#include <osgTerrain/Locator>
#include <osgTerrain/GeometryTechnique>
#include <sstream>
#include <stdlib.h>

using namespace osgEarth;

GeographicTileBuilder::GeographicTileBuilder( 
    MapConfig* _map,
    const std::string& _url_template,
    const osgDB::ReaderWriter::Options* _global_options ) :
TileBuilder( _map, _url_template, _global_options )
{
    //NOP
}

bool
GeographicTileBuilder::addChildren( osg::Group* tile_parent, const TileKey* key )
{
    osg::ref_ptr<osg::Node> q0 = createQuadrant(key->getSubkey(0));
    osg::ref_ptr<osg::Node> q1 = createQuadrant(key->getSubkey(1));
    osg::ref_ptr<osg::Node> q2;
    osg::ref_ptr<osg::Node> q3;

    bool allQuadrantsCreated = (q0.valid() && q1.valid());

    if ( key->getLevelOfDetail() > 0 || dynamic_cast<const MercatorTileKey*>( key ) )
    {
        q2 = createQuadrant( key->getSubkey( 2 ) );
        q3 = createQuadrant( key->getSubkey( 3 ) );
        allQuadrantsCreated = (allQuadrantsCreated && q2.valid() && q3.valid());
    }

    if (allQuadrantsCreated)
    {
        if (q0.valid()) tile_parent->addChild(q0.get());
        if (q1.valid()) tile_parent->addChild(q1.get());
        if (q2.valid()) tile_parent->addChild(q2.get());
        if (q3.valid()) tile_parent->addChild(q3.get());
    }
    else
    {
        osg::notify(osg::INFO) << "Couldn't create all 4 quadrants for " << key->str() << " time to stop subdividing!" << std::endl;
    }
    return allQuadrantsCreated;
}


osg::Node*
GeographicTileBuilder::createQuadrant( const TileKey* key )
{
    double min_lon, min_lat, max_lon, max_lat;
    if ( !key->getGeoExtents( min_lon, min_lat, max_lon, max_lat ) )
    {
        osg::notify( osg::WARN ) << "GET EXTENTS FAILED!" << std::endl;
        return NULL;
    }

    int tile_size = key->getProfile().pixelsPerTile();

    ImageTileList image_tiles;

    //Create the images
    std::vector<osg::ref_ptr<osg::Image> > images;
    //TODO: select/composite:
    if ( image_sources.size() > 0 )
    {
        //Add an image from each image source
        for (unsigned int i = 0; i < image_sources.size(); ++i)
        {
            ImageTileKeyPair image_tile(image_sources[i]->createImage(key), key);
            image_tiles.push_back(image_tile);
        }
    }

    //Create the heightfield for the tile
    osg::ref_ptr<osg::HeightField> hf = NULL;
    //TODO: select/composite.
    if ( heightfield_sources.size() > 0 )
    {
        hf = heightfield_sources[0]->createHeightField(key);
    }


    //Determine if we've created any images
    unsigned int numValidImages = 0;
    for (unsigned int i = 0; i < image_tiles.size(); ++i)
    {
        if (image_tiles[i].first.valid()) numValidImages++;
    }

    //If we couldn't create any imagery of heightfields, bail out
    if (!hf.valid() && (numValidImages == 0))
    {
        osg::notify(osg::INFO) << "Could not create any imagery or heightfields for " << key->str() <<".  Not building tile" << std::endl;
        return NULL;
    }
   
    //Try to interpolate any missing imagery from parent tiles
    for (unsigned int i = 0; i < image_sources.size(); ++i)
    {
        if (!image_tiles[i].first.valid())
        {
            if (!createValidImage(image_sources[i].get(), key, image_tiles[i]))
            {
                osg::notify(osg::WARN) << "Could not get valid image from image source " << i << " for TileKey " << key->str() << std::endl;
                return NULL;
            }
            else
            {
                osg::notify(osg::INFO) << "Interpolated imagery from image source " << i << " for TileKey " << key->str() << std::endl;
            }
        }
    }

    //Fill in missing heightfield information from parent tiles
    if (!hf.valid())
    {
        //We have no heightfield sources, 
        if (heightfield_sources.size() == 0)
        {
            //Make any empty heightfield if no heightfield source is specified
            hf = new osg::HeightField();
            hf->allocate( 8, 8 );
            for(unsigned int i=0; i<hf->getHeightList().size(); i++ )
                hf->getHeightList()[i] = 0.0; //(double)((::rand() % 10000) - 5000);
        }
        else
        {
            hf = createValidHeightField(heightfield_sources[0].get(), key);
            if (!hf.valid())
            {
                osg::notify(osg::WARN) << "Could not get valid heightfield for TileKey " << key->str() << std::endl;
                return NULL;
            }
            else
            {
                osg::notify(osg::INFO) << "Interpolated heightfield TileKey " << key->str() << std::endl;
            }
        }
    }

    //Scale the heightfield elevations from meters to degrees
    scaleHeightFieldToDegrees(hf.get());

    osgTerrain::Locator* geo_locator = new osgTerrain::Locator();
    geo_locator->setCoordinateSystemType( osgTerrain::Locator::GEOGRAPHIC ); // sort of.
    geo_locator->setTransformAsExtents( min_lon, min_lat, max_lon, max_lat );
    
    hf->setOrigin( osg::Vec3d( min_lon, min_lat, 0.0 ) );
    hf->setXInterval( (max_lon - min_lon)/(double)(hf->getNumColumns()-1) );
    hf->setYInterval( (max_lat - min_lat)/(double)(hf->getNumRows()-1) );
    hf->setBorderWidth( 0 );
    hf->setSkirtHeight( 0 );

    osgTerrain::HeightFieldLayer* hf_layer = new osgTerrain::HeightFieldLayer();
    hf_layer->setLocator( geo_locator );
    hf_layer->setHeightField( hf.get() );

    osgTerrain::TerrainTile* tile = new osgTerrain::TerrainTile();
    tile->setLocator( geo_locator );
    tile->setTerrainTechnique( new osgTerrain::GeometryTechnique() );
    tile->setElevationLayer( hf_layer );
    tile->setRequiresNormals( true );
    tile->setTileID(key->getTileId());

    //Attach an updatecallback to normalize the edges of TerrainTiles.
    tile->setUpdateCallback(new TerrainTileEdgeNormalizerUpdateCallback());
    tile->setDataVariance(osg::Object::DYNAMIC);

    for (unsigned int i = 0; i < image_tiles.size(); ++i)
    {
        if (image_tiles[i].first->valid())
        {
            double img_min_lon, img_min_lat, img_max_lon, img_max_lat;
            image_tiles[i].second->getGeoExtents(img_min_lon, img_min_lat, img_max_lon, img_max_lat);

            //Specify a new locator for the color with the coordinates of the TileKey that was actually used to create the image
            osg::ref_ptr<osgTerrain::Locator> img_locator = new osgTerrain::Locator;
            img_locator->setCoordinateSystemType( osgTerrain::Locator::GEOGRAPHIC);
            img_locator->setTransformAsExtents(img_min_lon, img_min_lat,img_max_lon, img_max_lat);

            // use a special image locator to warp the texture coords for mercator tiles :)
            // WARNING: TODO: this will not persist upon export....we need a nodekit.
            if ( dynamic_cast<const MercatorTileKey*>( key ) )
            {
                img_locator = new MercatorLocator(*img_locator.get(), tile_size, image_tiles[i].second->getLevelOfDetail() );
            }
            osgTerrain::ImageLayer* img_layer = new osgTerrain::ImageLayer( image_tiles[i].first.get());
            img_layer->setLocator( img_locator.get());

#if (OPENSCENEGRAPH_MAJOR_VERSION == 2 && OPENSCENEGRAPH_MINOR_VERSION < 7)
            img_layer->setFilter( osgTerrain::Layer::LINEAR );
#else
            img_layer->setMinFilter(osg::Texture::LINEAR_MIPMAP_LINEAR);
            img_layer->setMagFilter(osg::Texture::LINEAR);
#endif
            tile->setColorLayer( i, img_layer );
        }
    }
    
    osg::Vec3d centroid( (max_lon+min_lon)/2.0, (max_lat+min_lat)/2.0, 0 );

    double max_range = 1e10;
    double radius = (centroid-osg::Vec3d(min_lon,min_lat,0)).length();
    double min_range = radius * map->getMinTileRangeFactor();

    //Set the skirt height of the heightfield
    hf->setSkirtHeight(radius * map->getSkirtRatio());
   
    osg::PagedLOD* plod = new osg::PagedLOD();
    plod->setCenter( centroid );
    plod->addChild( tile, min_range, max_range );
    plod->setFileName( 1, createURI( key ) );
    plod->setRange( 1, 0.0, min_range );

    return plod;
}

osg::CoordinateSystemNode*
GeographicTileBuilder::createCoordinateSystemNode() const
{
    osg::CoordinateSystemNode* csn = new osg::CoordinateSystemNode();
    csn->setEllipsoidModel( NULL );
    csn->setCoordinateSystem( "+proj=eqc +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0" );
    csn->setFormat( "PROJ4" );
    return csn;
}

void
GeographicTileBuilder::scaleHeightFieldToDegrees(osg::HeightField *hf)
{
    //The number of degrees in a meter at the equator
    float scale = 1.0f/111319.0f;
    if (hf)
    {
        for (unsigned int i = 0; i < hf->getHeightList().size(); ++i)
        {
            hf->getHeightList()[i] *= scale;
        }
    }
    else
    {
        osg::notify(osg::WARN) << "scaleHeightFieldToDegrees heightfield is NULL" << std::endl;
    }
}