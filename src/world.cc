/*
 *  Stage : a multi-robot simulator.
 *  Copyright (C) 2001, 2002 Richard Vaughan, Andrew Howard and Brian Gerkey.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
/*
 * Desc: top level class that contains everything
 * Author: Richard Vaughan, Andrew Howard
 * Date: 7 Dec 2000
 * CVS info: $Id: world.cc,v 1.118 2002-08-30 18:17:28 rtv Exp $
 */
#if HAVE_CONFIG_H
  #include <config.h>
#endif
#if HAVE_STRINGS_H
  #include <strings.h>
#endif


//#undef DEBUG
//#undef VERBOSE
//#define DEBUG 
//#define VERBOSE

#include <errno.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <libgen.h>  // for dirname
#include <netdb.h>

#include <fstream>
#include <iostream>

#include "world.hh"
#include "playerdevice.hh"
#include "library.hh"

bool usage = false;

void PrintUsage(); // defined in main.cc
void StageQuit();

long int g_bytes_output = 0;
long int g_bytes_input = 0;

int g_timer_events = 0;

#ifdef INCLUDE_RTK2
// static var accessible to callbacks
int CWorld::autosubscribe = 0;
#endif

// allocate chunks of 32 pointers for entity storage
const int OBJECT_ALLOC_SIZE = 32;

// dummy timer signal func
void TimerHandler( int val )
{
  //puts( "TIMER HANDLER" );
  g_timer_events++;

  // re-install signal handler for timing
  if( signal( SIGALRM, &TimerHandler ) == SIG_ERR )
    {
      PRINT_ERR("failed to install signal handler");
      exit( -1 );
    }

  //printf( "\ng_timer_expired: %d\n", g_timer_expired );
}  

///////////////////////////////////////////////////////////////////////////
// Default constructor
CWorld::CWorld( int argc, char** argv, Library* lib )
{
  this->lib = lib;

  // seed the random number generator
  srand48( time(NULL) );

  // Initialise configuration variables
  this->ppm = 20;

  // matrix is created by a StageIO object
  this->matrix = NULL;

  // stop time of zero means run forever
  m_stoptime = 0;
  
  m_clock = 0;

  // invalid file descriptor initially
  m_log_fd = -1;

  m_external_sync_required = false;
  m_instance = 0;

  // just initialize stuff here
  this->root = NULL;

  this->entity_count = 0;

  rtp_player = NULL;

  m_log_output = false; // enable with -l <filename>
  m_console_output = false; // enable with -o
    
  // real time mode by default
  // if real_timestep is zero, we run as fast as possible
  m_real_timestep = 0.1; //seconds
  m_sim_timestep = 0.1; //seconds; - 10Hz default rate 
  m_step_num = 0;

  // start paused
  //
  m_enable = false;

  if( gethostname( m_hostname, sizeof(m_hostname)) == -1)
    {
      perror( "Stage: couldn't get hostname. Quitting." );
      exit( -1 );
    }

  /* now strip off domain */
  char* first_dot;
  strncpy(m_hostname_short, m_hostname,HOSTNAME_SIZE);
  if( (first_dot = strchr(m_hostname_short,'.') ))
    *first_dot = '\0';
      
  // get the IP of our host
  struct hostent* info = gethostbyname( m_hostname );
  
  if( info )
    { // make sure this looks like a regular internet address
      assert( info->h_length == 4 );
      assert( info->h_addrtype == AF_INET );
      
      // copy the address out
      memcpy( &m_hostaddr.s_addr, info->h_addr_list[0], 4 ); 
    }
  else
    {
      PRINT_ERR1( "failed to resolve IP for local hostname \"%s\"\n", 
		  m_hostname );
    }
  
  // Run the gui by default
  this->enable_gui = true;

  // default color database file
  strcpy( m_color_database_filename, COLOR_DATABASE );

  // Initialise clocks
  m_start_time = m_sim_time = 0;
  memset( &m_sim_timeval, 0, sizeof( struct timeval ) );
    
  // Initialise entity list
  this->entity_count = 0;
  
  // start with no key
  bzero(m_auth_key,sizeof(m_auth_key));
 
  // give the command line a chance to override the default values
  // we just set
  if( !ParseCmdLine( argc, argv )) 
    {
      quit = true;
      return;
    }
}


///////////////////////////////////////////////////////////////////////////
// Destructor
CWorld::~CWorld()
{
  if( matrix )
    delete matrix;

  if( root )
    delete root;
}

///////////////////////////////////////////////////////////////////////////
// Parse the command line
bool CWorld::ParseCmdLine(int argc, char **argv)
{
  for( int a=1; a<argc; a++ )
    {   
      // USAGE
      if( (strcmp( argv[a], "-?" ) == 0) || 
	  (strcmp( argv[a], "--help") == 0) )
	{
	  PrintUsage();
	  exit(0); // bail right here
	}
      
      // LOGGING
      if( strcmp( argv[a], "-l" ) == 0 )
	{
	  m_log_output = true;
	  strncpy( m_log_filename, argv[a+1], 255 );
	  printf( "[Logfile %s]", m_log_filename );
	
	  //store the command line for logging later
	  memset( m_cmdline, 0, sizeof(m_cmdline) );
	
	  for( int g=0; g<argc; g++ )
	    {
	      strcat( m_cmdline, argv[g] );
	      strcat( m_cmdline, " " );
	    }
	
	  a++;
	
	  // open the log file and write out a header
	  LogOutputHeader();
	}
    
      // DIS/ENABLE GUI
      if( strcmp( argv[a], "-g" ) == 0 )
	{
	  this->enable_gui = false;
	  printf( "[No GUI]" );
	}
    
      // SET GOAL REAL CYCLE TIME
      // Stage will attempt to update at this speed
      if( strcmp( argv[a], "-u" ) == 0 )
	{
	  m_real_timestep = atof(argv[a+1]);
	  printf( "[Real time per cycle %f sec]", m_real_timestep );
	  a++;
	}
      
      // SET SIMULATED UPDATE CYCLE
      // one cycle simulates this much time
      else if( strcmp( argv[a], "-v" ) == 0 )
	{
	  m_sim_timestep = atof(argv[a+1]);
	  printf( "[Simulated time per cycle %f sec]", m_sim_timestep );
	  a++;
	}
      
      // DISABLE console output
      if( strcmp( argv[a], "-o" ) == 0 )
	{
	  m_console_output = true;
	  printf( "[Console Output]" );
	}
      
      if(!strcmp(argv[a], "-t"))
	{
	  m_stoptime = atoi(argv[++a]);
	  printf("setting time to: %d\n",m_stoptime);
	}

      // ENABLE RTP - sensor data is sent in rtp format
      if( (strcmp( argv[a], "-r" ) == 0 ) || 
	  ( strcmp( argv[a], "--rtp" ) == 0 ))
	{
	  assert( rtp_player = new CRTPPlayer( argv[a+1] ) );
	
	  //printf( "World rtp player @ %p\n", rtp_player );
	  
      	  printf( "[RTP %s]", argv[a+1] );
      	  a++;
	  

	}
      //else if( strcmp( argv[a], "-id" ) == 0 )
      //{
      //  memset( m_hostname, 0, 64 );
      //  strncpy( m_hostname, argv[a+1], 64 );
      //  printf( "[ID %s]", m_hostname ); fflush( stdout );
      //  a++;
      //}
    }
    
  return true;
}


///////////////////////////////////////////////////////////////////////////
// Startup routine 
bool CWorld::Startup()
{  
  PRINT_DEBUG( "** STARTUP **" );
  
  // we must have at least one entity to play with!
  if( this->entity_count < 1 )
    {
      puts( "\nStage: No entities defined in world file. Nothing to simulate!" );
      return false;
    }

  // Initialise the real time clock
  // Note that we really do need to set the start time to zero first!
  m_start_time = 0;
  m_start_time = GetRealTime();
    
  // Initialise the rate counter
  m_update_ratio = 1;
  m_update_rate = 0;

#ifdef INCLUDE_RTK2 // Start the GUI
  if (this->enable_gui) RtkStartup();
#endif
  
  if( m_real_timestep > 0.0 ) // if we're in real-time mode
    StartTimer( m_real_timestep ); // start the real-time interrupts going
  
#ifdef DEBUG
  root->Print( "" );
#endif

  PRINT_DEBUG( "** STARTUP DONE **" );
  return true;
}
    

///////////////////////////////////////////////////////////////////////////
// Shutdown routine 
void CWorld::Shutdown()
{
  PRINT_DEBUG( "world shutting down" );

#ifdef INCLUDE_RTK2
  // Stop the GUI
  if (this->enable_gui)
    RtkShutdown();
#endif

  // Shutdown all the entities
  // Devices will unlink their device files
  root->Shutdown(); 
}




void CWorld::StartTimer( double interval )
{
  // set up the interval timer
  //
  // set a timer to go off every few ms. in realtime mode we'll sleep
  // in between if there's nothing else to do. 

  //install signal handler for timing
  if( signal( SIGALRM, &TimerHandler ) == SIG_ERR )
    {
      PRINT_ERR("failed to install signal handler");
      exit( -1 );
    }

  //printf( "interval: %f\n", interval );

  //start timer with chosen interval (specified in milliseconds)
  struct itimerval tick;
  // seconds
  tick.it_value.tv_sec = tick.it_interval.tv_sec = (long)floor(interval);
  // microseconds
  tick.it_value.tv_usec = tick.it_interval.tv_usec = 
    (long)fmod( interval * MILLION, MILLION); 
  
  if( setitimer( ITIMER_REAL, &tick, 0 ) == -1 )
    {
      PRINT_ERR("failed to set timer");
      exit( -1 );
    }
}


///////////////////////////////////////////////////////////////////////////
// Update the world
void CWorld::Update(void)
{
  //PRINT_DEBUG( "** Update **" );
  //assert( arg == 0 );
  
  //while( !quit )
    
      // if the sim isn't running, we pause briefly and return
      if( !m_enable )
	{
	  usleep( 100000 );
	  return;
	}

      // is it time to stop?
      if(m_stoptime && m_sim_time >= m_stoptime)
	{   
	  //system("kill `cat stage.pid`");
	  quit = true;
	  return;
	}
      
      // otherwise we're running - calculate new world state
      
      // let the entities do anything they want to do between clock increments
      root->Sync(); 
      
      // if the timer has gone off recently or we're in fast mode
      // we increment the clock and do the time-based updates
      if( g_timer_events > 0 || m_real_timestep == 0 )
	{          
	  
	  // Update the simulation time (in both formats)
	  m_sim_time = m_step_num * m_sim_timestep;
	  m_sim_timeval.tv_sec = (long)floor(m_sim_time);
	  m_sim_timeval.tv_usec = (long)((m_sim_time-floor(m_sim_time)) * MILLION); 
	  
	  // export the time - copy the timeval into the player io buffer
	  if( m_clock ) // if we're managing a clock
	    {
	      // TODO - move this into the server?
	      sem_wait( &m_clock->lock );
	      m_clock->time = m_sim_timeval;
	      sem_post( &m_clock->lock );
	    }
	  
	  // update the entities managed by this host at this time 
	  root->Update( m_sim_time );	  
	  
#ifdef INCLUDE_RTK2   
	  // also update the gui at this rate
	  if (this->enable_gui) RtkUpdate();
#endif	      
	  

	  if( g_timer_events > 0 )
	    g_timer_events--; // we've handled this timer event
	  
	  // increase the time step counter
	  m_step_num++; 
	}
      
      
      Output(); // perform console and log output
      
      // if there's nothing pending and we're not in fast mode, we let go
      // of the processor (on linux gives us around a 10ms cycle time)
      if( g_timer_events < 1 && m_real_timestep > 0.0 ) 
	usleep( 0 );
      
      // dump the contents of the matrix to a file for debugging
      //world->matrix->dump();
      //getchar();	
}


///////////////////////////////////////////////////////////////////////////
// Get the sim time
// Returns time in sec since simulation started
double CWorld::GetTime()
{
  return m_sim_time;
}


///////////////////////////////////////////////////////////////////////////
// Get the real time
// Returns time in sec since simulation started
double CWorld::GetRealTime()
{
  struct timeval tv;
  gettimeofday( &tv, NULL );
  double time = tv.tv_sec + (tv.tv_usec / 1000000.0);
  return time - m_start_time;
}


///////////////////////////////////////////////////////////////////////////
// Set a rectangle in the world grid
void CWorld::SetRectangle(double px, double py, double pth,
                          double dx, double dy, CEntity* ent, bool add)
{
  Rect rect;

  dx /= 2.0;
  dy /= 2.0;

  double cx = dx * cos(pth);
  double cy = dy * cos(pth);
  double sx = dx * sin(pth);
  double sy = dy * sin(pth);
    
  rect.toplx = (int) ((px + cx - sy) * ppm);
  rect.toply = (int) ((py + sx + cy) * ppm);

  rect.toprx = (int) ((px + cx + sy) * ppm);
  rect.topry = (int) ((py + sx - cy) * ppm);

  rect.botlx = (int) ((px - cx - sy) * ppm);
  rect.botly = (int) ((py - sx + cy) * ppm);

  rect.botrx = (int) ((px - cx + sy) * ppm);
  rect.botry = (int) ((py - sx - cy) * ppm);
    
  //printf( "draw_rect %d,%d %d,%d %d,%d %d,%d\n",
  //  rect.toplx, rect.toply,
  //  rect.toprx, rect.topry,
  //  rect.botlx, rect.botly,
  //  rect.botrx, rect.botry );

  matrix->draw_rect( rect, ent, add );
}


///////////////////////////////////////////////////////////////////////////
// Set a circle in the world grid
void CWorld::SetCircle(double px, double py, double pr, CEntity* ent,
                       bool add )
{
  // Convert from world to image coords
  int x = (int) (px * ppm);
  int y = (int) (py * ppm);
  int r = (int) (pr * ppm);
    
  matrix->draw_circle( x,y,r,ent, add);
}


///////////////////////////////////////////////////////////////////////////
// Add an entity to the world
//
void CWorld::RegisterEntity( CEntity *entity)
{
  // store the pointer at the current index in the vector
  child_vector.push_back( entity );
  this->entity_count++;
}


// returns true if the given hostname matches our hostname, false otherwise
//  bool CWorld::CheckHostname(char* host)
//  {
//    //printf( "checking %s against (%s and %s) ", 
//    //  host, m_hostname, m_hostname_short ); 

//    if(!strcmp(m_hostname,host) || !strcmp(m_hostname_short,host))
//    {
//      //PRINT_DEBUG( "TRUE" );
//      return true;
//    }
//    else
//    {
//      //PRINT_DEBUG( "FALSE" );
//      return false;
//    }
//  }


// sleep until a signal goes off
// return the time in seconds we spent asleep
double CWorld::Pause()
{
  // we're too busy to sleep!
  if( m_real_timestep == 0 || --g_timer_events > 0  )
    return 0;
  
  // otherwise

  double sleep_start = GetRealTime();

  pause(); // wait for the signal

  return( GetRealTime() - sleep_start );
}


void CWorld::Output()
{
  // comms used
  static unsigned long last_input = 0;
  static unsigned long last_output = 0;
  unsigned int bytes_in = g_bytes_input - last_input;
  unsigned int bytes_out = g_bytes_output - last_output;
  static int bytes_accumulator = 0;
  
  // count the data
  bytes_accumulator += bytes_in + bytes_out;

  // measure frequency & bandwidth
  static double freq = 0.0;
  static double bandw = 0.0;

  static int updates = 0;
  static double lasttime = GetRealTime();
  double interval = GetRealTime() - lasttime;

  // count this update
  updates++;
  
  if( interval > 2.0 ) // measure freq + bandwidth every 2 seconds
    {
      lasttime += interval;

      bandw = (double)bytes_accumulator / interval;
      bytes_accumulator = 0;
      
      freq = (double)updates / interval;
      updates = 0;    
    }
  
  if( m_console_output )
    ConsoleOutput( freq, bytes_in, bytes_out, bandw );
  
  
  if( m_log_output ) 
    LogOutput( freq, bytes_in, bytes_out, g_bytes_input, g_bytes_output );
  
  last_input = g_bytes_input;
  last_output = g_bytes_output; 
}

void CWorld::ConsoleOutput( double freq, 
			    unsigned int bytes_in, unsigned int bytes_out,
			    double avg_data)
{
  printf( " Time: %8.1f - %7.1fHz - [%4u/%4u] %8.2f b/sec\r", 
  //printf( "\n Time: %8.1f - %7.1fHz - [%4u/%4u] %8.2f b/sec\n", 
	  m_sim_time, 
          freq,
	  bytes_in, bytes_out, 
          avg_data );
  
  fflush( stdout );
  
}


void CWorld::LogOutput( double freq,
			unsigned int bytes_in, unsigned int bytes_out, 
			unsigned int total_bytes_in, 
			unsigned int total_bytes_out )
{  
  assert( m_log_fd > 0 );
  
  char line[512];
  sprintf( line,
           "%u\t\t%.3f\t%u\t%u\t%u\t%u\n", 
           m_step_num, m_sim_time, // step and time
           //loop_duration, // real cycle time in ms
           //sleep_duration, // real sleep time in ms
           //m_sim_timestep / sleep_duration, // ratio
           bytes_in, // bytes in this cycle
           bytes_out, // bytes out this cycle
           total_bytes_in,  // total bytes in
           total_bytes_out); // total bytes out
  
  write( m_log_fd, line, strlen(line) );
}


void CWorld::LogOutputHeader( void )  
{
  int log_instance = 0;
  while( m_log_fd < 0 )
    {
      char fname[256];
      sprintf( fname, "%s.%d", m_log_filename, log_instance++ );
      m_log_fd = open( fname, O_CREAT | O_EXCL | O_WRONLY, 
		       S_IREAD | S_IWRITE );
    }

  struct timeval t;
  gettimeofday( &t, 0 );
      
  // count the locally managed entities
  int m=0;
  //CHILDLOOP( ent ) if( ent->m_local ) m++;
      
  char* tmstr = ctime( &t.tv_sec);
  tmstr[ strlen(tmstr)-1 ] = 0; // delete the newline
      
  char line[512];
  sprintf( line,
           "# Stage output log\n#\n"
           "# Command:\t%s\n"
           "# Date:\t\t%s\n"
           "# Host:\t\t%s\n"
           //"# Bitmap:\t%s\n"
           "# Timestep(ms):\t%d\n"
           "# Entities:\t%d of %d\n#\n"
           "#STEP\t\tSIMTIME(s)\tINTERVAL(s)\tSLEEP(s)\tRATIO\t"
           "\tINPUT\tOUTPUT\tITOTAL\tOTOTAL\n",
           m_cmdline, 
           tmstr, 
           m_hostname, 
           //worldfilename,
           (int)(m_sim_timestep * 1000.0),
           m, 
           this->entity_count );
      
  write( m_log_fd, line, strlen(line) );
}


#ifdef INCLUDE_RTK2


void CWorld::AddToMenu( stage_menu_t* menu, CEntity* ent, int check )
{
  assert( menu );
  assert( ent );

  // if there's no menu item for this type yet
  if( menu->items[ ent->stage_type ] == NULL )
    // create a new menu item
    assert( menu->items[ ent->stage_type ] =  
	    rtk_menuitem_create( menu->menu, 
				 this->lib->
				 StringFromType(ent->stage_type),1));
  
  
  rtk_menuitem_check( menu->items[ ent->stage_type ], check );
}

void CWorld::AddToDataMenu(  CEntity* ent, int check )
{
  assert( ent );
  AddToMenu( &this->data_menu, ent, check );
}

void CWorld::AddToDeviceMenu(  CEntity* ent, int check )
{
  assert( ent );
  AddToMenu( &this->device_menu, ent, check );
}

  // devices check this to see if they should display their data
bool CWorld::ShowDeviceData( StageType devtype )
{ 
  rtk_menuitem_t* menu_item = data_menu.items[ devtype ];
  
  if( menu_item )
    return( rtk_menuitem_ischecked( menu_item ) );  
  else // if there's no option in the menu, display this data
    return true;
}

bool CWorld::ShowDeviceBody( StageType devtype )
{
  rtk_menuitem_t* menu_item = device_menu.items[ devtype ];
  
  if( menu_item )
    return( rtk_menuitem_ischecked( menu_item ) );  
  else // if there's no option in the menu, display this data
    return true;    
}

// Initialise the GUI
// TODO: fix this for client/server operation.
bool CWorld::RtkLoad(CWorldFile *worldfile)
{
  int sx, sy;
  double scale = 0.01;
  double dx, dy;
  double ox, oy;
  double gridx, gridy;
  double minor, major;
  bool showgrid;
  bool subscribedonly;

  if (worldfile != NULL)
    {
      int section = worldfile->LookupEntity("gui");

      // Size of world in pixels
      sx = (int) this->matrix->width;
      sy = (int) this->matrix->height;

      // Grid size in meters
      gridx = sx / this->ppm;
      gridy = sy / this->ppm;

      // Place a hard limit, just to stop it going off the screen
      // (TODO - we could get the sceen size from X if we tried?)
      if (sx > 1024)
	sx = 1024;
      if (sy > 768)
	sy = 768;

      // Size of canvas in pixels
      sx = (int) worldfile->ReadTupleFloat(section, "size", 0, sx);
      sy = (int) worldfile->ReadTupleFloat(section, "size", 1, sy);
    
      // Scale of the pixels
      scale = worldfile->ReadLength(section, "scale", 1 / this->ppm);
  
      // Size in meters
      dx = sx * scale;
      dy = sy * scale;

      // Origin of the canvas
      ox = worldfile->ReadTupleLength(section, "origin", 0, dx / 2);
      oy = worldfile->ReadTupleLength(section, "origin", 1, dy / 2);


      // Grid spacing
      minor = worldfile->ReadTupleLength(section, "grid", 0, 0.2);
      major = worldfile->ReadTupleLength(section, "grid", 1, 1.0);
      showgrid = worldfile->ReadInt(section, "showgrid", true);
    
      // toggle display of subscribed or all device data
      subscribedonly = worldfile->ReadInt(section, "showsubscribed", false);

      gridx = ceil(gridx / major) * major;
      gridy = ceil(gridy / major) * major;
    }
  else
    {
      // Size of world in pixels
      sx = (int) this->matrix->width;
      sy = (int) this->matrix->height;

      // Grid size in meters
      gridx = sx / this->ppm;
      gridy = sy / this->ppm;

      // Place a hard limit, just to stop it going off the screen
      if (sx > 1024)
	sx = 1024;
      if (sy > 768)
	sy = 768;
    
      // Size in meters
      dx = sx * scale;
      dy = sy * scale;

      // Origin of the canvas
      ox = dx / 2;
      oy = dy / 2;


      // Grid spacing
      minor = 0.2;
      major = 1.0;
      showgrid = true;

      // default
      subscribedonly = true;

      gridx = ceil(gridx / major) * major;
      gridy = ceil(gridy / major) * major;
    }
  
  this->app = rtk_app_create();
  rtk_app_refresh_rate(this->app, 10);
  
  this->canvas = rtk_canvas_create(this->app);
  rtk_canvas_size(this->canvas, sx, sy);
  rtk_canvas_scale(this->canvas, scale, scale);
  rtk_canvas_origin(this->canvas, ox, oy);

  // Add some menu items
  this->file_menu = rtk_menu_create(this->canvas, "File");
  this->save_menuitem = rtk_menuitem_create(this->file_menu, "Save", 0);
  this->export_menuitem = rtk_menuitem_create(this->file_menu, "Export", 0);
  this->exit_menuitem = rtk_menuitem_create(this->file_menu, "Exit", 0);
  this->export_count = 0;

  // Create the view menu
  this->view_menu = rtk_menu_create(this->canvas, "View");

  // create the view menu items and set their initial checked state
  this->grid_item = rtk_menuitem_create(this->view_menu, "Grid", 1);
  rtk_menuitem_check(this->grid_item, showgrid);

  this->matrix_item = rtk_menuitem_create(this->view_menu, "Matrix", 1);
  rtk_menuitem_check(this->matrix_item, 0);

  // create the action menu
  this->action_menu = rtk_menu_create(this->canvas, "Action");
  this->subscribedonly_item = rtk_menuitem_create(this->action_menu, 
						  "Subscribe to all", 1);

  rtk_menuitem_check(this->subscribedonly_item, subscribedonly);

  this->autosubscribe_item = rtk_menuitem_create(this->action_menu, 
						    "Subscribe to selection", 1);
  rtk_menuitem_check(this->autosubscribe_item, 0 );


  //zero the view menus
  memset( &device_menu,0,sizeof(stage_menu_t));
  memset( &data_menu,0,sizeof(stage_menu_t));

  // Create the view/device sub menu
  assert( this->data_menu.menu = 
	  rtk_menu_create_sub(this->view_menu, "Data"));

  // Create the view/data sub menu
  assert( this->device_menu.menu = 
	  rtk_menu_create_sub(this->view_menu, "Object"));

  // each device adds itself to the correct view menus in its rtkstartup()
  
  
  // Create the grid
  this->fig_grid = rtk_fig_create(this->canvas, NULL, -49);
  if (minor > 0)
    {
      rtk_fig_color(this->fig_grid, 0.9, 0.9, 0.9);
      rtk_fig_grid(this->fig_grid, gridx/2, gridy/2, gridx, gridy, minor);
    }
  if (major > 0)
    {
      rtk_fig_color(this->fig_grid, 0.75, 0.75, 0.75);
      rtk_fig_grid(this->fig_grid, gridx/2, gridy/2, gridx, gridy, major);
    }
  rtk_fig_show(this->fig_grid, showgrid);
  
  return true;
}

// Save the GUI
bool CWorld::RtkSave(CWorldFile *worldfile)
{
  int section = worldfile->LookupEntity("gui");
  if (section < 0)
    {
      PRINT_WARN("No gui entity in the world file; gui settings have not been saved.");
      return true;
    }

  // Size of canvas in pixels
  int sx, sy;
  rtk_canvas_get_size(this->canvas, &sx, &sy);
  worldfile->WriteTupleFloat(section, "size", 0, sx);
  worldfile->WriteTupleFloat(section, "size", 1, sy);

  // Origin of the canvas
  double ox, oy;
  rtk_canvas_get_origin(this->canvas, &ox, &oy);
  worldfile->WriteTupleLength(section, "origin", 0, ox);
  worldfile->WriteTupleLength(section, "origin", 1, oy);

  // Scale of the canvas
  double scale;
  rtk_canvas_get_scale(this->canvas, &scale, &scale);
  worldfile->WriteLength(section, "scale", scale);

  // Grid on/off
  int showgrid = rtk_menuitem_ischecked(this->grid_item);
  worldfile->WriteInt(section, "showgrid", showgrid);
  
  return true;
}


// Start the GUI
bool CWorld::RtkStartup()
{
  PRINT_DEBUG( "** STARTUP GUI **" );

  // don't call this
  //rtk_app_start(this->app);

  // create the main objects here
  rtk_canvas_t *canvas;
  rtk_table_t *table;

  // this rtkstarts all entities
  root->RtkStartup();
  
  // Display everything
  for (canvas = app->canvas; canvas != NULL; canvas = canvas->next)
    gtk_widget_show_all(canvas->frame);
  for (table = app->table; table != NULL; table = table->next)
    gtk_widget_show_all(table->frame);


  return true;
}


// Stop the GUI
void CWorld::RtkShutdown()
{
  rtk_app_stop(this->app);
}


// Update the GUI
void CWorld::RtkUpdate()
{
  RtkMenuHandling();      

  // this is from rtk_on_app_timer
  rtk_canvas_t *canvas;
  rtk_table_t *table;
  
  // Quit the app if we have been told we should
  // We first destroy in windows that are still open.
  if (app->must_quit)
    {
      for (canvas = app->canvas; canvas != NULL; canvas = canvas->next)
	if (!canvas->destroyed)
	  gtk_widget_destroy(canvas->frame);
      for (table = app->table; table != NULL; table = table->next)
	if (!table->destroyed)
	  gtk_widget_destroy(table->frame);
      gtk_main_quit();
    }
  
  // update the object tree
  root->RtkUpdate();
  
  
  // Update the display
  for (canvas = app->canvas; canvas != NULL; canvas = canvas->next)
    rtk_canvas_render(canvas, FALSE, NULL);
  
  //struct timeval tv;
  //gettimeofday( &tv, NULL );
  //double start = tv.tv_sec + tv.tv_usec / 1000000.0;
  
  // allow gtk to do some work
  while( gtk_events_pending () )
    gtk_main_iteration ();
  
  //gettimeofday( &tv, NULL );

   //double duration = (tv.tv_sec + tv.tv_usec / 1000000.0) - start;

  //printf( "gtkloop: %.4f\n", duration );
}

// Update the GUI
void CWorld::RtkMenuHandling()
{
  // right now we have to check the state of all the menu items each
  // time around the loop. this is pretty unsatisfactory - callbacks
  // would be much better

  // See if we need to quit the program
  if (rtk_menuitem_isactivated(this->exit_menuitem))
    ::quit = 1;
  if (rtk_canvas_isclosed(this->canvas))
    ::quit = 1;

  // Save the world file
  if (rtk_menuitem_isactivated(this->save_menuitem))
    SaveFile(NULL);

  // Handle export menu item
  // TODO - fold in XS's postscript and pnm export here or in rtk2
  if (rtk_menuitem_isactivated(this->export_menuitem))
    {
      char filename[128];
      snprintf(filename, sizeof(filename), 
	       "rtkstage-%04d.fig", this->export_count++);
      PRINT_MSG1("exporting canvas to [%s]", filename);
      rtk_canvas_export(this->canvas, filename);
    }

  // Show or hide the grid
  if (rtk_menuitem_ischecked(this->grid_item))
    rtk_fig_show(this->fig_grid, 1);
  else
    rtk_fig_show(this->fig_grid, 0);

  // clear any matrix rendering, then redraw if the emnu item is checked
  this->matrix->unrender();
  if (rtk_menuitem_ischecked(this->matrix_item))
    this->matrix->render( this );
  
 
  // enable/disable automatic subscription to the selected device
  CWorld::autosubscribe = rtk_menuitem_ischecked(this->autosubscribe_item);
  
  // enable/disable subscriptions to show sensor data
  static bool lasttime = rtk_menuitem_ischecked(this->subscribedonly_item);
  bool thistime = rtk_menuitem_ischecked(this->subscribedonly_item);

  // for now i check if the menu item changed
  if( thistime != lasttime )
    {
      if( thistime )  // change the subscription counts of any player-capable ent
	{
	  for( vector<CEntity*>::iterator it= child_vector.begin();
	       it != child_vector.end();
	       it++ )
	    if( RTTI_ISPLAYERP( *it ) ) 
	      dynamic_cast<CPlayerEntity*>(*it)->Subscribe();
	}
      else
	{
	  for( vector<CEntity*>::iterator it= child_vector.begin();
	       it != child_vector.end();
	       it++ )
	    if( RTTI_ISPLAYERP( *it ) ) 
	      dynamic_cast<CPlayerEntity*>(*it)->Unsubscribe();
	}
      // remember this state
      lasttime = thistime;
    }
      
  
}
#endif


