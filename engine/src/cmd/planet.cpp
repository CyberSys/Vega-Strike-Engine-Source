/**
 * planet.cpp
 *
 * Copyright (C) Daniel Horn
 * Copyright (C) 2020 pyramid3d, Stephen G. Tuggy, and other Vega Strike
 * contributors
 *
 * https://github.com/vegastrike/Vega-Strike-Engine-Source
 *
 * This file is part of Vega Strike.
 *
 * Vega Strike is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Vega Strike is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Vega Strike.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <math.h>
#include "vegastrike.h"
#include "unit.h"
#include "planet.h"
#include "gfxlib.h"
#include "gfx/sphere.h"
#include "collection.h"
#include "ai/order.h"
#include "gfxlib_struct.h"
#include "vs_globals.h"
#include "config_xml.h"
#include <assert.h>
#include "cont_terrain.h"
#include "atmosphere.h"
#ifdef FIX_TERRAIN
#include "gfx/planetary_transform.h"
#endif

#include "collide2/CSopcodecollider.h"
#include "images.h"
#include "gfx/halo.h"
#include "gfx/animation.h"
#include "cmd/script/flightgroup.h"
#include "gfx/ring.h"
#include "alphacurve.h"
#include "gfx/vsimage.h"
#include "vsfilesystem.h"
#include "gfx/camera.h"
#include "universe.h"
#include "configuration/game_config.h"
#include "planetary_orbit.h"
#include "universe_util.h"
#include "damageable_factory.h"

using std::endl;

extern float ScaleJumpRadius( float );
extern Flightgroup * getStaticBaseFlightgroup( int faction );
extern bool CrashForceDock( Unit *thus, Unit *dockingUn, bool force );
extern void abletodock( int dock );

//////////////////////////////////////////////
// Functions
//////////////////////////////////////////////

char * getnoslash( char *inp )
{
    char *tmp = inp;
    for (unsigned int i = 0; inp[i] != '\0'; i++)
        if (inp[i] == '/' || inp[i] == '\\')
            tmp = inp+i+1;
    return tmp;
}

string getCargoUnitName( const char *textname )
{
    char *tmp2 = strdup( textname );
    char *tmp  = getnoslash( tmp2 );
    unsigned int i;
    for (i = 0; tmp[i] != '\0' && (isalpha( tmp[i] ) || tmp[i] == '_'); i++) {}
    if (tmp[i] != '\0')
        tmp[i] = '\0';
    string retval( tmp );
    free( tmp2 );
    return retval;
}

string GetElMeshName( string name, string faction, char direction )
{
    using namespace VSFileSystem;
    char    strdir[2]     = {direction, 0};
    string  elxmesh       = string( strdir )+"_elevator.bfxm";
    string  elevator_mesh = name+"_"+faction+elxmesh;
    VSFile  f;
    VSError err = f.OpenReadOnly( elevator_mesh, MeshFile );
    if (err > Ok)
        f.Close();
    else elevator_mesh = name+elxmesh;
    return elevator_mesh;
}

static void SetFogMaterialColor( Mesh *thus, const GFXColor &color, const GFXColor &dcolor )
{
    static float emm  = GameConfig::GetVariable( "graphics", "atmosphere_emmissive", 1.0f);
    static float diff = XMLSupport::parse_float( vs_config->getVariable( "graphics", "atmosphere_diffuse", "1" ) );
    GFXMaterial  m;
    setMaterialAmbient( m, 0.0);
    setMaterialDiffuse( m, diff*dcolor);
    setMaterialSpecular( m, 0.0);
    setMaterialEmissive(m, emm*color);
    m.power = 0;
    thus->SetMaterial( m );
}
Mesh * MakeFogMesh( const AtmosphericFogMesh &f, float radius )
{
    static int count = 0;
    count++;
    string     nam   = f.meshname+XMLSupport::tostring( count )+".png";
    if (f.min_alpha != 0 || f.max_alpha != 255 || f.concavity != 0 || f.focus != .5 || f.tail_mode_start != -1
        || f.tail_mode_end != -1) {
        static int     rez = XMLSupport::parse_int( vs_config->getVariable( "graphics", "atmosphere_texture_resolution", "512" ) );
        unsigned char *tex = (unsigned char*) malloc( sizeof (char)*rez*4 );
        for (int i = 0; i < rez; ++i) {
            tex[i*4]   = 255;
            tex[i*4+1] = 255;
            tex[i*4+2] = 255;
            tex[i*4+3] = get_alpha( i, rez, f.min_alpha, f.max_alpha, f.focus, f.concavity, f.tail_mode_start, f.tail_mode_end );
        }
        //Writing in the homedir texture directory
        ::VSImage image;
        using VSFileSystem::TextureFile;
        image.WriteImage( (char*) nam.c_str(), &tex[0], PngImage, rez, 1, true, 8, TextureFile );
    }
    vector< string >override;
    override.push_back( nam );
    Mesh *ret = Mesh::LoadMesh( f.meshname.c_str(), Vector( f.scale*radius, f.scale*radius, f.scale*radius ), 0, nullptr, override );
    ret->setConvex( true );
    SetFogMaterialColor( ret, GFXColor( f.er, f.eg, f.eb, f.ea ), GFXColor( f.dr, f.dg, f.db, f.da ) );
    return ret;
}

//////////////////////////////////////////////
// AtmosphereHalo
//////////////////////////////////////////////
class AtmosphereHalo : public GameUnit
{
public:
    float planetRadius;
    AtmosphereHalo( float radiusOfPlanet, vector< Mesh* > &meshes, int faction ) :
        GameUnit ( meshes, true, faction )
    {
        planetRadius = radiusOfPlanet;
    }
    virtual void Draw( const Transformation &quat = identity_transformation, const Matrix &m = identity_matrix )
    {
        QVector dirtocam   = _Universe->AccessCamera()->GetPosition()-m.p;
        Transformation qua = quat;
        Matrix  mat = m;
        float   distance   = dirtocam.Magnitude();

        float   MyDistanceRadiusFactor = planetRadius/distance;
        float HorizonHeight = sqrt( 1-MyDistanceRadiusFactor*MyDistanceRadiusFactor )*planetRadius;

        float zscale;
        float xyscale = zscale = HorizonHeight/planetRadius;
        zscale = 0;
        dirtocam.Normalize();
        mat.p += sqrt( planetRadius*planetRadius-HorizonHeight*HorizonHeight )*dirtocam;
        ScaleMatrix( mat, Vector( xyscale, xyscale, zscale ) );
        qua.position = mat.p;
        GameUnit::Draw( qua, mat );
    }
};

//////////////////////////////////////////////
// Constructors and the like
//////////////////////////////////////////////
Planet::Planet() :
    GameUnit ( 0 )
{
    Init();
    SetAI( new Order() );     //no behavior

    /* TODO: lib_damage
    shield.number=2;
    shield.recharge=0;
    shield.shield2fb.frontmax=0;
    shield.shield2fb.backmax=0;
    shield.shield2fb.front=0;
    shield.shield2fb.back=0;*/
    Health health(2,0,0);
    layers[2] = DamageableFactory::CreateLayer(2, FacetConfiguration::two, health, false);
    hull = &layers[0];
    armor = &layers[1];
    shield = &layers[2];
}

extern const vector< string >& ParseDestinations( const string &value );
Planet::Planet( QVector x,
                        QVector y,
                        float vely,
                        const Vector &rotvel,
                        float pos,
                        float gravity,
                        float radius,
                        const string &textname,
                        const string &technique,
                        const string &unitname,
                        BLENDFUNC blendSrc,
                        BLENDFUNC blendDst,
                        const vector< string > &dest,
                        const QVector &orbitcent,
                        Unit *parent,
                        const GFXMaterial &ourmat,
                        const std::vector< GFXLightLocal > &ligh,
                        int faction,
                        string fgid,
                        bool inside_out ) :
    GameUnit ( 0 )
{
    atmosphere = nullptr;
    atmospheric  = false;

    inside = false;
    shine = nullptr;
    terrain    = nullptr;
    terraintrans = nullptr;



    unsigned int nlights = 0;
    if ( !ligh.empty() )
        nlights = ligh.size();
    for (unsigned int i = 0; i < nlights; i++) {
        int l;
        GFXCreateLight( l, ligh[i].ligh, !ligh[i].islocal );
        lights.push_back( l );
    }
    bool wormhole = dest.size() != 0;
    if (wormhole) {
        static std::string wormhole_unit = vs_config->getVariable( "graphics", "wormhole", "wormhole" );
        string stab( ".stable" );
        if (rand() > RAND_MAX*.99)
            stab = ".unstable";
        string wormholename   = wormhole_unit+stab;
        string wormholeneutralname = wormhole_unit+".neutral"+stab;
        Unit  *jum = new GameUnit( wormholename.c_str(), true, faction );
        int    neutralfaction = FactionUtil::GetNeutralFaction();
        faction = neutralfaction;

        Unit  *neujum  = new GameUnit( wormholeneutralname.c_str(), true, neutralfaction );
        Unit  *jump    = jum;
        bool   anytrue = false;
        while (jump != nullptr) {
            if (jump->name != "LOAD_FAILED") {
                anytrue = true;
                radius  = jump->rSize();
                Mesh *shield = jump->meshdata.size() ? jump->meshdata.back() : nullptr;
                if ( jump->meshdata.size() ) jump->meshdata.pop_back();
                while ( jump->meshdata.size() ) {
                    this->meshdata.push_back( jump->meshdata.back() );
                    jump->meshdata.pop_back();
                }
                jump->meshdata.push_back( shield );
                for (un_iter i = jump->getSubUnits(); !i.isDone(); ++i)
                    SubUnits.prepend( *i );
                jump->SubUnits.clear();
            }
            jump->Kill();
            if (jump != neujum)
                jump = neujum;
            else
                jump = nullptr;
        }
        if (anytrue)
            meshdata.push_back( nullptr );              //shield mesh...otherwise is a standard planet
        wormhole = anytrue;
    }
    if (!wormhole) {
        static int stacks = XMLSupport::parse_int( vs_config->getVariable( "graphics", "planet_detail", "24" ) );
        atmospheric = !(blendSrc == ONE && blendDst == ZERO);
        meshdata.push_back( new SphereMesh( radius, stacks, stacks, textname.c_str(), technique, nullptr, inside_out, blendSrc, blendDst ) );
        meshdata.back()->setEnvMap( GFXFALSE );
        meshdata.back()->SetMaterial( ourmat );
        meshdata.push_back( nullptr );
    }
    calculate_extent( false );
    if (wormhole) {
        static float radscale = XMLSupport::parse_float( vs_config->getVariable( "physics", "jump_mesh_radius_scale", ".5" ) );
        radius *= radscale;
        corner_min.i = corner_min.j = corner_min.k = -radius;
        corner_max.i = corner_max.j = corner_max.k = radius;
        radial_size  = radius;
        if ( !meshdata.empty() )
            meshdata[0]->setVirtualBoundingBox( corner_min, corner_max, radius );
    }
    if (ligh.size() > 0) {
        static float bodyradius = XMLSupport::parse_float( vs_config->getVariable( "graphics", "star_body_radius", ".33" ) );
        static bool  drawglow   = XMLSupport::parse_bool( vs_config->getVariable( "graphics", "draw_star_glow", "true" ) );

        static bool  drawstar   = XMLSupport::parse_bool( vs_config->getVariable( "graphics", "draw_star_body", "true" ) );
        static float glowradius =
            XMLSupport::parse_float( vs_config->getVariable( "graphics", "star_glow_radius", "1.33" ) )/bodyradius;
        if (drawglow) {
            GFXColor    c = getMaterialEmissive( ourmat );
            static bool spec = XMLSupport::parse_bool( vs_config->getVariable( "graphics", "glow_ambient_star_light", "false" ) );
            static bool diff = XMLSupport::parse_bool( vs_config->getVariable( "graphics", "glow_diffuse_star_light", "false" ) );
            if (diff)
                c = ligh[0].ligh.GetProperties( DIFFUSE );
            if (spec)
                c = ligh[0].ligh.GetProperties( AMBIENT );
            static vector< string >shines = ParseDestinations( vs_config->getVariable( "graphics", "star_shine", "shine.ani" ) );
            if ( shines.empty() )
                shines.push_back( "shine.ani" );
            shine = new Animation( shines[rand()%shines.size()].c_str(), true, .1, BILINEAR, false, true, c );             //GFXColor(ourmat.er,ourmat.eg,ourmat.eb,ourmat.ea));
            shine->SetDimensions( glowradius*radius, glowradius*radius );
            if (!drawstar) {
                delete meshdata[0];
                meshdata.clear();
                meshdata.push_back( nullptr );
            }
        }
    }
    this->InitPlanet( x, y, vely, rotvel,
                      pos,
                      gravity, radius,
                      textname, technique, unitname,
                      dest,
                      orbitcent, parent,
                      faction, fgid,
                      inside_out,
                      nlights );

    /* TODO: lib_damage
    shield.number=2;
    shield.recharge=0;
    shield.shield2fb.frontmax=0;
    shield.shield2fb.backmax=0;
    shield.shield2fb.front=0;
    shield.shield2fb.back=0;*/
    Health health(2,0,0);
    layers[2] = DamageableFactory::CreateLayer(2, FacetConfiguration::two, health, false);
    hull = &layers[0];
    armor = &layers[1];
    shield = &layers[2];
}

Planet::~Planet()
{
    if (shine)
        delete shine;
    if (terrain)
        delete terrain;
    if (atmosphere)
        delete atmosphere;
#ifdef FIX_TERRAIN
    if (terraintrans) {
        Matrix *tmp = new Matrix();
        *tmp = cumulative_transformation_matrix;
        terraintrans->SetTransformation( tmp );
        //FIXME
        //We're losing memory here...but alas alas... planets don't die that often
    }
#endif
}

void Planet::InitPlanet( QVector x,
                         QVector y,
                         float vely,
                         const Vector &rotvel,
                         float pos,
                         float gravity,
                         float radius,
                         const string &filename,
                         const string &technique,
                         const string &unitname,
                         const vector< string > &dest,
                         const QVector &orbitcent,
                         Unit *parent,
                         int faction,
                         string fullname,
                         bool inside_out,
                         unsigned int lights_num )
{
    static const float bodyradius = GameConfig::GetVariable( "graphics", "star_body_radius", 0.33f );

    if (lights_num)
        radius *= bodyradius;

    curr_physical_state.position = prev_physical_state.position = cumulative_transformation.position = orbitcent+x;
    Init();
    //static int neutralfaction=FactionUtil::GetFaction("neutral");
    //this->faction = neutralfaction;
    killed = false;
    bool notJumppoint = dest.empty();
    for (unsigned int i = 0; i < dest.size(); ++i)
        AddDestination( dest[i] );
    //name = "Planet - ";
    //name += textname;
    name = fullname;
    this->fullname = name;
    this->radius   = radius;
    this->gravity  = gravity;
    static float densityOfRock = XMLSupport::parse_float( vs_config->getVariable( "physics", "density_of_rock", "3" ) );
    static float densityOfJumpPoint =
        XMLSupport::parse_float( vs_config->getVariable( "physics", "density_of_jump_point", "100000" ) );
    //static  float massofplanet = XMLSupport::parse_float(vs_config->getVariable("physics","mass_of_planet","10000000"));
    *current_hull = (4./3)*M_PI*radius*radius*radius*(notJumppoint ? densityOfRock : densityOfJumpPoint);
    this->Mass   = (4./3)*M_PI*radius*radius*radius*( notJumppoint ? densityOfRock : (densityOfJumpPoint/100000) );
    SetAI( new PlanetaryOrbit( this, vely, pos, x, y, orbitcent, parent ) );     //behavior
    terraintrans = nullptr;

    colTrees     = nullptr;
    SetAngularVelocity( rotvel );
    // The docking port is 20% bigger than the planet
    static float planetdockportsize    = XMLSupport::parse_float( vs_config->getVariable( "physics", "planet_port_size", "1.2" ) );
    static float planetdockportminsize =
        XMLSupport::parse_float( vs_config->getVariable( "physics", "planet_port_min_size", "300" ) );
    if ( (!atmospheric) && notJumppoint ) {
        float dock = radius*planetdockportsize;
        if (dock-radius < planetdockportminsize)
            dock = radius+planetdockportminsize;
        pImage->dockingports.push_back( DockingPorts( Vector( 0, 0, 0 ), dock, 0, DockingPorts::Type::CONNECTED_OUTSIDE ) );
    }
    string tempname = unitname.empty() ? ::getCargoUnitName( filename.c_str() ) : unitname;
    setFullname( tempname );

    int    tmpfac   = faction;
    if (UniverseUtil::LookupUnitStat( tempname, FactionUtil::GetFactionName( faction ), "Cargo_Import" ).length() == 0)
        tmpfac = FactionUtil::GetPlanetFaction();
    Unit  *un = new GameUnit( tempname.c_str(), true, tmpfac );

    static bool smartplanets = XMLSupport::parse_bool( vs_config->getVariable( "physics", "planets_can_have_subunits", "false" ) );
    if ( un->name != string( "LOAD_FAILED" ) ) {
        cargo = un->cargo;
        CargoVolume   = un->CargoVolume;
        UpgradeVolume = un->UpgradeVolume;
        VSSprite *tmp = pImage->pHudImage;
        pImage->pHudImage     = un->GetImageInformation().pHudImage;
        un->GetImageInformation().pHudImage = tmp;
        maxwarpenergy = un->warpCapData();
        if (smartplanets) {
            SubUnits.prepend( un );
            un->SetRecursiveOwner( this );
            this->SetTurretAI();
            un->SetTurretAI();              //allows adding planetary defenses, also allows launching fighters from planets, interestingly
            un->name = "Defense_grid";
        }
        static bool neutralplanets =
            XMLSupport::parse_bool( vs_config->getVariable( "physics", "planets_always_neutral", "true" ) );
        if (neutralplanets) {
            static int neutralfaction = FactionUtil::GetNeutralFaction();
            this->faction = neutralfaction;
        } else {
            this->faction = faction;
        }
    }
    if ( un->name == string( "LOAD_FAILED" ) || (!smartplanets) )
        un->Kill();
}

//////////////////////////////////////////////
// Methods
//////////////////////////////////////////////

void Planet::AddAtmosphere( const std::string &texture,
                                float radius,
                                BLENDFUNC blendSrc,
                                BLENDFUNC blendDst,
                                bool inside_out )
{
    if ( meshdata.empty() )
        meshdata.push_back( nullptr );
    Mesh *shield = meshdata.back();
    meshdata.pop_back();
    static int stacks = XMLSupport::parse_int( vs_config->getVariable( "graphics", "planet_detail", "24" ) );
    meshdata.push_back( new SphereMesh( radius, stacks, stacks, texture.c_str(), string(), nullptr, inside_out, blendSrc, blendDst ) );
    if ( meshdata.back() ) {
        //By klauss - this needs to be done for most atmospheres
        GFXMaterial a = {
            0, 0, 0, 0,
            1, 1, 1, 1,
            0, 0, 0, 0,
            0, 0, 0, 0,
            0
        };
        meshdata.back()->SetMaterial( a );
    }
    meshdata.push_back( shield );
}

void Planet::AddCity( const std::string &texture,
                          float radius,
                          int numwrapx,
                          int numwrapy,
                          BLENDFUNC blendSrc,
                          BLENDFUNC blendDst,
                          bool inside_out,
                          bool reverse_normals )
{
    if ( meshdata.empty() )
        meshdata.push_back( nullptr );
    Mesh *shield = meshdata.back();
    meshdata.pop_back();
    static float materialweight    = XMLSupport::parse_float( vs_config->getVariable( "graphics", "city_light_strength", "10" ) );
    static float daymaterialweight = XMLSupport::parse_float( vs_config->getVariable( "graphics", "day_city_light_strength", "0" ) );
    GFXMaterial  m;
    setMaterialAmbient( m, 0.0);
    setMaterialDiffuse( m, materialweight );
    setMaterialSpecular(m, 0.0);
    setMaterialEmissive(m, daymaterialweight );
    m.power = 0.0;
    static int stacks = XMLSupport::parse_int( vs_config->getVariable( "graphics", "planet_detail", "24" ) );
    meshdata.push_back( new CityLights( radius, stacks, stacks, texture.c_str(), numwrapx, numwrapy, inside_out, ONE, ONE,
                                        false, 0, M_PI, 0.0, 2*M_PI, reverse_normals ) );
    meshdata.back()->setEnvMap( GFXFALSE );
    meshdata.back()->SetMaterial( m );

    meshdata.push_back( shield );
}

void Planet::AddFog( const std::vector< AtmosphericFogMesh > &v, bool opticalillusion )
{
    if ( meshdata.empty() ) meshdata.push_back( nullptr );
#ifdef MESHONLY
    Mesh *shield = meshdata.back();
    meshdata.pop_back();
#endif
    std::vector< Mesh* >fogs;
    for (unsigned int i = 0; i < v.size(); ++i) {
        Mesh *fog = MakeFogMesh( v[i], rSize() );
        fogs.push_back( fog );
    }
    Unit *fawg;
    if (opticalillusion)
        fawg = new AtmosphereHalo( this->rSize(), fogs, 0 );
    else
        fawg = new GameUnit( fogs, true, 0 );
    fawg->setFaceCamera();
    getSubUnits().preinsert( fawg );
    *fawg->current_hull /= fawg->GetHullPercent();
#ifdef MESHONLY
    meshdata.push_back( shield );
#endif
}

void Planet::AddRing( const std::string &texture,
                          float iradius,
                          float oradius,
                          const QVector &R,
                          const QVector &S,
                          int slices,
                          int wrapx,
                          int wrapy,
                          BLENDFUNC blendSrc,
                          BLENDFUNC blendDst )
{
    if ( meshdata.empty() )
        meshdata.push_back( nullptr );
    Mesh *shield = meshdata.back();
    meshdata.pop_back();
    static int stacks = XMLSupport::parse_int( vs_config->getVariable( "graphics", "planet_detail", "24" ) );
    if (slices > 0) {
        stacks = stacks;
        if (stacks < 3)
            stacks = 3;
        for (int i = 0; i < slices; i++)
            meshdata.push_back( new RingMesh( iradius, oradius, stacks, texture.c_str(), R, S, wrapx, wrapy, blendSrc, blendDst,
                                             false, i*(2*M_PI)/( (float) slices ), (i+1)*(2*M_PI)/( (float) slices ) ) );
    }
    meshdata.push_back( shield );
}

void Planet::AddSatellite( Unit *orbiter )
{
    satellites.prepend( orbiter );
    orbiter->SetOwner( this );
}

Vector Planet::AddSpaceElevator( const std::string &name, const std::string &faction, char direction )
{
    Vector dir, scale;
    switch (direction)
    {
    case 'u':
        dir.Set( 0, 1, 0 );
        break;
    case 'd':
        dir.Set( 0, -1, 0 );
        break;
    case 'l':
        dir.Set( -1, 0, 0 );
        break;
    case 'r':
        dir.Set( 1, 0, 0 );
        break;
    case 'b':
        dir.Set( 0, 0, -1 );
        break;
    default:
        dir.Set( 0, 0, 1 );
        break;
    }
    Matrix ElevatorLoc( Vector( dir.j, dir.k, dir.i ), dir, Vector( dir.k, dir.i, dir.j ) );
    scale = dir*radius+Vector( 1, 1, 1 )-dir;
    Mesh  *shield = meshdata.back();
    string elevator_mesh = GetElMeshName( name, faction, direction );     //filename
    Mesh  *tmp    = meshdata.back() = Mesh::LoadMesh( elevator_mesh.c_str(),
                                                      scale,
                                                      FactionUtil::
                                                      GetFactionIndex( faction ),
                                                      nullptr );

    meshdata.push_back( shield );
    {
        //subunit computations
        Vector mn( tmp->corner_min() );
        Vector mx( tmp->corner_max() );
        if (dir.Dot( Vector( 1, 1, 1 ) ) > 0)
            ElevatorLoc.p.Set( dir.i*mx.i, dir.j*mx.j, dir.k*mx.k );
        else
            ElevatorLoc.p.Set( -dir.i*mn.i, -dir.j*mn.j, -dir.k*mn.k );
        Unit *un = new GameUnit( name.c_str(), true, FactionUtil::GetFactionIndex( faction ), "", nullptr );
        if (pImage->dockingports.back().GetPosition().MagnitudeSquared() < 10)
            pImage->dockingports.clear();
        pImage->dockingports.push_back( DockingPorts( ElevatorLoc.p, un->rSize()*1.5, 0, DockingPorts::Type::INSIDE ) );
        un->SetRecursiveOwner( this );
        un->SetOrientation( ElevatorLoc.getQ(), ElevatorLoc.getR() );
        un->SetPosition( ElevatorLoc.p );
        SubUnits.prepend( un );
    }
    return dir;
}

///////////////////////////////////////////////////////////////////////


vector< UnitContainer* >PlanetTerrainDrawQueue;
void Planet::Draw( const Transformation &quat, const Matrix &m )
{
    //Do lighting fx
    //if cam inside don't draw?
    //if(!inside) {
    GameUnit::Draw( quat, m );
    //}
    QVector    t( _Universe->AccessCamera()->GetPosition()-Position() );
    static int counter = 0;
    if (counter++ > 100) {
        if (t.Magnitude() < corner_max.i) {
            inside = true;
        } else {
            inside = false;
            ///somehow warp unit to reasonable place outisde of planet
            if (terrain) {
#ifdef PLANETARYTRANSFORM
                terrain->DisableUpdate();
#endif
            }
        }
    }
    GFXLoadIdentity( MODEL );
    for (unsigned int i = 0; i < lights.size(); i++)
        GFXSetLight( lights[i], POSITION, GFXColor( cumulative_transformation.position.Cast() ) );
    if (inside && terrain)
        PlanetTerrainDrawQueue.push_back( new UnitContainer( this ) );
    if (shine) {
        Vector  p, q, r;
        QVector c;
        MatrixToVectors( cumulative_transformation_matrix, p, r, q, c );
        shine->SetOrientation( p, q, r );
        shine->SetPosition( c );
        static int num_shine_drawing =
            XMLSupport::parse_int( vs_config->getVariable( "graphics", "num_times_to_draw_shine", "2" ) );
        for (int i = 0; i < num_shine_drawing; ++i)
            shine->Draw();
    }
}

void Planet::DrawTerrain()
{
    inside = true;
    if (terrain)
        terrain->EnableUpdate();
#ifdef PLANETARYTRANSFORM
    TerrainUp = t;
    Normalize( TerrainUp );
    TerrainH  = TerrainUp.Cross( Vector( -TerrainUp.i+.25, TerrainUp.j-.24, -TerrainUp.k+.24 ) );
    Normalize( TerrainH );
#endif

    GFXLoadIdentity( MODEL );
    if (inside && terrain) {
        _Universe->AccessCamera()->UpdatePlanetGFX();
        terrain->SetTransformation( *_Universe->AccessCamera()->GetPlanetGFX() );
        terrain->AdjustTerrain( _Universe->activeStarSystem() );
        terrain->Draw();
#ifdef PLANETARYTRANSFORM
        terraintrans->GrabPerpendicularOrigin( _Universe->AccessCamera()->GetPosition(), tmp );
        terrain->SetTransformation( tmp );
        terrain->AdjustTerrain( _Universe->activeStarSystem() );
        terrain->Draw();
        if (atmosphere) {
            Vector tup( tmp[4], tmp[5], tmp[6] );
            Vector p    = ( _Universe->AccessCamera()->GetPosition() );
            Vector blah = p-Vector( tmp[12], tmp[13], tmp[14] );
            blah    = p-( blah.Dot( tup ) )*tup;
            tmp[12] = blah.i;
            tmp[13] = blah.j;
            tmp[14] = blah.k;
            atmosphere->SetMatricesAndDraw( _Universe->AccessCamera()->GetPosition(), tmp );
        }
#endif
    }
}

///////////////////////////////////////////////////////////////////////

Unit* Planet::beginElement( QVector x,
                            QVector y,
                            float vely,
                            const Vector &rotvel,
                            float pos,
                            float gravity,
                            float radius,
                            const string &filename,
                            const string &technique,
                            const string &unitname,
                            BLENDFUNC blendSrc,
                            BLENDFUNC blendDst,
                            const vector< string > &dest,
                            int level,
                            const GFXMaterial &ourmat,
                            const vector< GFXLightLocal > &ligh,
                            bool isunit,
                            int faction,
                            string fullname,
                            bool inside_out )
{
    //this function is OBSOLETE
    Unit *un = nullptr;
    if (level > 2) {
        un_iter satiterator = satellites.createIterator();
        assert( *satiterator );
        if ( (*satiterator)->isUnit() == _UnitType::planet ) {
            un = ( (Planet*) (*satiterator) )->beginElement( x, y, vely, rotvel, pos,
                                                             gravity, radius,
                                                             filename, technique, unitname,
                                                             blendSrc, blendDst,
                                                             dest,
                                                             level-1,
                                                             ourmat, ligh,
                                                             isunit,
                                                             faction, fullname,
                                                             inside_out );
        } else {
            BOOST_LOG_TRIVIAL(error) << "Planets are unable to orbit around units";
        }
    } else {
        if (isunit == true) {
            Unit *sat_unit  = nullptr;
            Flightgroup *fg = getStaticBaseFlightgroup( faction );
            satellites.prepend( sat_unit = new GameUnit( filename.c_str(), false, faction, "", fg, fg->nr_ships-1 ) );
            sat_unit->setFullname( fullname );
            un = sat_unit;
            un_iter satiterator( satellites.createIterator() );
            (*satiterator)->SetAI( new PlanetaryOrbit( *satiterator, vely, pos, x, y, QVector( 0, 0, 0 ), this ) );
            (*satiterator)->SetOwner( this );
        } else {
            // For debug
//            BOOST_LOG_TRIVIAL(trace) << "name" << " : " << filename << " : " << unitname << endl;
//            BOOST_LOG_TRIVIAL(trace) << "R/X: " << x.i << " : " << x.j << " : " << x.k << endl;
//            BOOST_LOG_TRIVIAL(trace) << "S/Y: " << y.i << " : " << y.j << " : " << y.k << endl;
//            BOOST_LOG_TRIVIAL(trace) << "CmpRotVel: " << rotvel.i << " : " <<
//                    rotvel.j << " : " << rotvel.k << endl;
//            BOOST_LOG_TRIVIAL(trace) << vely << " : " << pos << " : " << gravity << " : " << radius << endl;
//            BOOST_LOG_TRIVIAL(trace) << dest.size() << " : " << "orbit_center" << " : " << ligh.size() << endl;
//            BOOST_LOG_TRIVIAL(trace) << blendSrc << " : " << blendDst << " : " << inside_out << endl;




            Planet *p;
            if (dest.size() != 0)
                radius = ScaleJumpRadius( radius );
            satellites.prepend( p = new Planet( x, y, vely, rotvel, pos, gravity, radius,
                                                               filename, technique, unitname,
                                                               blendSrc, blendDst, dest,
                                                               QVector( 0, 0, 0 ), this, ourmat, ligh, faction, fullname, inside_out ) );
            un = p;
            p->SetOwner( this );
            BOOST_LOG_TRIVIAL(trace) << "Created planet " << fullname << " of type " << p->fullname << " orbiting " << this->fullname << endl;

        }
    }
    return un;
}

void Planet::DisableLights()
{
    for (unsigned int i = 0; i < lights.size(); i++)
        GFXDisableLight( lights[i] );
}

void Planet::EnableLights()
{
    for (unsigned int i = 0; i < lights.size(); i++)
        GFXEnableLight( lights[i] );
}



void Planet::endElement() {}

string Planet::getHumanReadablePlanetType() const
{
    //static std::map<std::string, std::string> planetTypes (readPlanetTypes("planet_types.xml"));
    //return planetTypes[getCargoUnitName()];
    return _Universe->getGalaxy()->getPlanetNameFromTexture( getCargoUnitName() );
}

Planet* Planet::GetTopPlanet( int level )
{
    if (level > 2) {
        un_iter satiterator = satellites.createIterator();
        assert( *satiterator );
        if ( (*satiterator)->isUnit() == _UnitType::planet ) {
            return ( (Planet*) (*satiterator) )->GetTopPlanet( level-1 );
        } else {
            BOOST_LOG_TRIVIAL(error) << "Planets are unable to orbit around units";
            return nullptr;
        }
    } else {
        return this;
    }
}

void Planet::Kill( bool erasefromsave )
{
    Unit *tmp;
    for (un_iter iter = satellites.createIterator(); (tmp=*iter)!=nullptr; ++iter)
        tmp->SetAI( new Order );
    /* probably not FIXME...right now doesn't work on paged out systems... not a big deal */
    for (unsigned int i = 0; i < this->lights.size(); i++)
        GFXDeleteLight( lights[i] );
    /*	*/
    satellites.clear();
    insiders.clear();
    GameUnit::Kill( erasefromsave );
}


void Planet::ProcessTerrains()
{
    while ( !PlanetTerrainDrawQueue.empty() ) {
        Planet *pl = (Planet*) PlanetTerrainDrawQueue.back()->GetUnit();
        pl->DrawTerrain();
        PlanetTerrainDrawQueue.back()->SetUnit( nullptr );
        delete PlanetTerrainDrawQueue.back();
        PlanetTerrainDrawQueue.pop_back();
    }
}

void Planet::setAtmosphere( Atmosphere *t )
{
    atmosphere = t;
}

PlanetaryTransform* Planet::setTerrain( ContinuousTerrain *t, float ratiox, int numwraps, float scaleatmos )
{
    terrain = t;
    terrain->DisableDraw();
    float x, z;
    t->GetTotalSize( x, z );
#ifdef FIX_TERRAIN
    terraintrans = new PlanetaryTransform( .8*corner_max.i, x*ratiox, z, numwraps, scaleatmos );
    terraintrans->SetTransformation( &cumulative_transformation_matrix );

    return terraintrans;
#endif
    return nullptr;
}

//////////////////////////////////////////////////////////////////////



bool operator==(const Planet& lhs, const Planet& rhs)
{
    bool equal = true;
    if(lhs.inside != rhs.inside) {
        equal = false;
        BOOST_LOG_TRIVIAL(trace) << "inside: " << lhs.inside << " != " << rhs.inside << endl;
    }

    if(lhs.atmospheric != rhs.atmospheric) {
        equal = false;
        BOOST_LOG_TRIVIAL(trace) << "atmospheric: " << lhs.atmospheric << " != " << rhs.atmospheric << endl;
    }

    // TODO: turn floating point comparisons into a function
    if(std::fabs(lhs.radius - rhs.radius) > 0.001f) {
        equal = false;
        BOOST_LOG_TRIVIAL(trace) << "radius: " << lhs.radius << " != " << rhs.radius << endl;
    }

    if(std::fabs(lhs.gravity - rhs.gravity) > 0.001f) {
        equal = false;
        BOOST_LOG_TRIVIAL(trace) << "gravity: " << lhs.gravity << " != " << rhs.gravity << endl;
    }

    return equal;
}

