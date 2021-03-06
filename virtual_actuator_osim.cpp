/////////////////////////////////////////////////////////////////////////////////////
//                                                                                 //
//  Copyright (c) 2016-2019 Leonardo Consoni <leonardojc@protonmail.com>           //
//                                                                                 //
//  This file is part of Robot-Control-OpenSim.                                    //
//                                                                                 //
//  Robot-Control-OpenSim is free software: you can redistribute it and/or modify  //
//  it under the terms of the GNU Lesser General Public License as published       //
//  by the Free Software Foundation, either version 3 of the License, or           //
//  (at your option) any later version.                                            //
//                                                                                 //
//  Robot-Control-OpenSim is distributed in the hope that it will be useful,       //
//  but WITHOUT ANY WARRANTY; without even the implied warranty of                 //
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                   //
//  GNU Lesser General Public License for more details.                            //
//                                                                                 //
//  You should have received a copy of the GNU Lesser General Public License       //
//  along with Robot-Control-OpenSim. If not, see <http://www.gnu.org/licenses/>.  //
//                                                                                 //
/////////////////////////////////////////////////////////////////////////////////////


#include "signal_io/signal_io.h"

#include <OpenSim/OpenSim.h>
#include <simbody/internal/Visualizer_InputListener.h>

#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <vector>
#include <exception>

#define DEVICES_NUMBER 4

enum OutputVariable { POSITION, VELOCITY, ACCELERATION, FORCE, VARS_NUMBER };

static const SimTK::Vec3 BLOCK_COLORS[ DEVICES_NUMBER ] = { SimTK::Blue, SimTK::Red, SimTK::Green, SimTK::Yellow };

typedef struct OSimDevice
{
  std::string name;
  OpenSim::Body* body;
  OpenSim::CoordinateActuator* userActuator;
  OpenSim::CoordinateActuator* controlActuator;
  OpenSim::Coordinate* coordinate;
  std::vector<double> measuresList;
  double controlForce;
}
OSimDevice;
  
static OpenSim::Model* model = nullptr; 
static SimTK::State state;

static std::vector<OpenSim::CoordinateActuator*> actuatorsList;

static std::thread updateThread;
static volatile bool isUpdating;

static std::vector<OSimDevice*> devicesList;

OSimDevice* CreateOSimDevice( OpenSim::Model* model, SimTK::Vec3 color, int index )
{    
  std::cout << "creating osim device" << std::endl;
  
  OSimDevice* device = new OSimDevice;
  
  std::string indexString = std::to_string( index + 1 );
  device->name = "device_" + indexString;
  
  device->body = new OpenSim::Body( "body_" + indexString, 1.0, SimTK::Vec3( 0, 0, 0 ), SimTK::Inertia( 1, 1, 1 ) );
  model->addBody( device->body );
  
  const double BODY_SIZE = 0.5;
  const double BODY_DISTANCE = 3 * BODY_SIZE;
  
  const OpenSim::Ground& ground = model->getGround();
  OpenSim::PinJoint* pinJoint = new OpenSim::PinJoint( "joint_" + indexString, 
                                                       ground, SimTK::Vec3( 0, BODY_DISTANCE, 0 ), SimTK::Vec3( 0, 0, 0 ), 
                                                       *(device->body), SimTK::Vec3( 0, 0, 0 ), SimTK::Vec3( 0, 0, 0 ) );
  model->addJoint( pinJoint );
  
  OpenSim::Brick* blockMesh = new OpenSim::Brick( SimTK::Vec3( BODY_SIZE, BODY_SIZE, BODY_SIZE ) );
  blockMesh->setColor( BLOCK_COLORS[ index ] );
  device->body->attachGeometry( blockMesh );
  
  OpenSim::Coordinate& coordinate = pinJoint->updCoordinate();
  device->userActuator = new OpenSim::CoordinateActuator( coordinate.getName() );
  model->addForce( device->userActuator );
  device->controlActuator = new OpenSim::CoordinateActuator( coordinate.getName() );
  model->addForce( device->controlActuator );
  
  device->measuresList.resize( VARS_NUMBER );
  
  device->controlForce = 0.0;
  
  return device;
}

static void Update()
{
  const long TIME_STEP_MS = 10;
  
  std::chrono::steady_clock::time_point initialTime = std::chrono::steady_clock::now();
  
  isUpdating = true;
  while( isUpdating )
  {
    std::chrono::steady_clock::time_point simulationTime = std::chrono::steady_clock::now();
    
    while( model->updVisualizer().updInputSilo().isAnyUserInput() ) 
    {
      int sliderID;
      SimTK::Real forceValue;
      while( model->updVisualizer().updInputSilo().takeSliderMove( sliderID, forceValue ) )
        actuatorsList[ sliderID ]->setOverrideActuation( state, forceValue );
      std::cout << "input from " << sliderID << ": " << forceValue << std::endl;
    }
    
    for( int deviceIndex = 0; deviceIndex < devicesList.size(); deviceIndex++ )
    {
      devicesList[ deviceIndex ]->controlActuator->setOverrideActuation( state, devicesList[ deviceIndex ]->controlForce );
      model->updVisualizer().updSimbodyVisualizer().setSliderValue( deviceIndex * 2 + 1, devicesList[ deviceIndex ]->controlForce ); 
    }
    
    OpenSim::Manager manager( *model );
    manager.initialize( state );
    state = manager.integrate( std::chrono::duration_cast<std::chrono::duration<double>>( simulationTime - initialTime ).count() );
    
    for( OSimDevice* device: devicesList )
    {
      device->measuresList[ POSITION ] = device->userActuator->getCoordinate()->getValue( state );
      device->measuresList[ VELOCITY ] = device->userActuator->getCoordinate()->getSpeedValue( state );
      device->measuresList[ ACCELERATION ] = device->userActuator->getCoordinate()->getAccelerationValue( state );
      device->measuresList[ FORCE ] = device->userActuator->getOverrideActuation( state );
    }
    
    std::this_thread::sleep_until( simulationTime + std::chrono::milliseconds( TIME_STEP_MS ) );
  }
}

DECLARE_MODULE_INTERFACE( SIGNAL_IO_INTERFACE );

long int InitDevice( const char* deviceName )
{  
  if( model == nullptr )
  {
    try
    {
      std::cout << "creating osim process" << std::endl;
      model = new OpenSim::Model();
      model->setUseVisualizer( true );
      
      model->setName( "osim_robot" );
      model->setGravity( SimTK::Vec3( 0, 0, 0 ) );

      for( size_t deviceIndex = 0; deviceIndex < DEVICES_NUMBER; deviceIndex++ )
        devicesList.push_back( CreateOSimDevice( model, BLOCK_COLORS[ deviceIndex ], deviceIndex ) );
    
      state = model->initSystem();
      
      for( OSimDevice* device: devicesList )
      {
        std::string sliderIndexString = std::to_string( actuatorsList.size() / 2 + 1 );
        model->updVisualizer().updSimbodyVisualizer().addSlider( "User Force " + sliderIndexString , actuatorsList.size(), -1.0, 1.0, 0.0 );
        actuatorsList.push_back( device->userActuator );
        model->updVisualizer().updSimbodyVisualizer().addSlider( "Control Force " + sliderIndexString, actuatorsList.size(), -2.0, 2.0, 0.0 );
        actuatorsList.push_back( device->controlActuator );
        
        device->userActuator->overrideActuation( state, true );
        device->controlActuator->overrideActuation( state, true );
      
        device->userActuator->getCoordinate()->setValue( state, 0.0 );
      }    
      
      state.setTime( 0.0 );
      
      std::cout << "starting update thread" << std::endl;
      updateThread = std::thread( Update );
    }
    catch( const OpenSim::Exception& exception )
    {
      std::cerr << exception.getMessage() << std::endl;
    }
    catch( const std::exception& exception )
    {
      std::cerr << exception.what() << std::endl;
    }
  }
  
  for( long int deviceIndex = 0; deviceIndex < devicesList.size(); deviceIndex++ )
    if( devicesList[ deviceIndex ]->name == deviceName ) return deviceIndex;

  return SIGNAL_IO_DEVICE_INVALID_ID;
}

void EndDevice( long int processID )
{
  isUpdating = false;
  updateThread.join();
    
  model->setUseVisualizer( false );
  
  delete model;
  
  for( OSimDevice* device : devicesList )
    delete device;
  
  return;
}

size_t GetMaxInputSamplesNumber( long int deviceID )
{
  return 1;
}

size_t Read( long int deviceID, unsigned int channel, double* ref_value )
{
  *ref_value = 0.0;
  
  size_t deviceIndex = (size_t) deviceID;
  if( deviceIndex > devicesList.size() ) return 0;
  
  if( channel >= VARS_NUMBER ) return 0;
  
  *ref_value = devicesList[ deviceIndex ]->measuresList[ channel ];
  
  return 1;
}

bool HasError( long int deviceID )
{
  return false;
}

void Reset( long int deviceID )
{
  return;
}

bool CheckInputChannel( long int deviceID, unsigned int channel )
{
  size_t deviceIndex = (size_t) deviceID;
  if( deviceIndex > devicesList.size() ) return false;
  
  if( channel >= VARS_NUMBER ) return false;
  
  return true;
}

bool Write( long int deviceID, unsigned int channel, double value )
{
  size_t deviceIndex = (size_t) deviceID;
  if( deviceIndex > devicesList.size() ) return false;
  
  if( channel != 0 ) return false;
  
  devicesList[ deviceIndex ]->controlForce = value;
  
  return true;
}

bool AcquireOutputChannel( long int taskID, unsigned int channel )
{
  return true;
}

void ReleaseOutputChannel( long int taskID, unsigned int channel )
{
  return;
}
