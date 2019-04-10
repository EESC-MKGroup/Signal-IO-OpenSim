////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (c) 2016-2019 Leonardo Consoni <consoni_2519@hotmail.com>       //
//                                                                            //
//  This file is part of RobotSystem-Lite.                                    //
//                                                                            //
//  RobotSystem-Lite is free software: you can redistribute it and/or modify  //
//  it under the terms of the GNU Lesser General Public License as published  //
//  by the Free Software Foundation, either version 3 of the License, or      //
//  (at your option) any later version.                                       //
//                                                                            //
//  RobotSystem-Lite is distributed in the hope that it will be useful,       //
//  but WITHOUT ANY WARRANTY; without even the implied warranty of            //
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the              //
//  GNU Lesser General Public License for more details.                       //
//                                                                            //
//  You should have received a copy of the GNU Lesser General Public License  //
//  along with RobotSystem-Lite. If not, see <http://www.gnu.org/licenses/>.  //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////


#include "signal_io/signal_io.h"

#include <OpenSim/OpenSim.h>
#include <simbody/internal/Visualizer_InputListener.h>

#include <iostream>
#include <cstdlib>
#include <chrono>
#include <vector>

const SimTK::Vec3 BLOCK_COLORS[] = { SimTK::Blue, SimTK::Red, SimTK::Green, SimTK::Yellow }; 

class OSimProcess;

static std::vector<OSimProcess*> processList;

class SliderListener : public SimTK::Visualizer::InputListener
{
public:
  SliderListener( OpenSim::CoordinateActuator* actuator, SimTK::State& state )
  : state( state )
  {
    this->actuator = actuator;
  }
  
  bool sliderMoved( int slider, SimTK::Real value )
  {
    if( slider != 1 ) return false;
    actuator->setOverrideActuation( state, value );
    return true;
  }
  
private:
  OpenSim::CoordinateActuator* actuator;
  SimTK::State& state;
};

class OSimProcess
{
public:  
  OSimProcess( const char* modelName, SimTK::Vec3 modelColor )
  {
    std::cout << "creating osim process" << std::endl;
    model = new OpenSim::Model();
  
    model->setUseVisualizer( true );
    
    model->setName( modelName );
    model->setGravity( SimTK::Vec3( 0, 0, 0 ) );

    body = new OpenSim::Body( "body", 1.0, SimTK::Vec3( 0, 0, 0 ), SimTK::Inertia( 0, 0, 0 ) );
    model->addBody( body );

    const OpenSim::Ground& ground = model->getGround();
    OpenSim::PinJoint* groundJoint = new OpenSim::PinJoint( "body2ground", ground, *body );
    //OpenSim::SliderJoint* groundJoint = new OpenSim::SliderJoint( "body2ground", ground, *body );
    model->addJoint( groundJoint );
  
    OpenSim::Brick* blockMesh = new OpenSim::Brick( SimTK::Vec3( 0.5, 0.5, 0.5 ) );
    blockMesh->setColor( modelColor );
    body->attachGeometry( blockMesh );
  
    OpenSim::Coordinate& coordinate = groundJoint->updCoordinate();
    inputActuator = new OpenSim::CoordinateActuator( "input" );
    inputActuator->setCoordinate( &coordinate );
    model->addForce( inputActuator );
    feedbackActuator = new OpenSim::CoordinateActuator( "feedback" );
    feedbackActuator->setCoordinate( &coordinate );
    model->addForce( feedbackActuator );
    
    state = model->initSystem();

    model->updVisualizer().updSimbodyVisualizer().addInputListener( new SliderListener( inputActuator, state ) );
    model->updVisualizer().updSimbodyVisualizer().addSlider( "input_force", 1, -1.0, 1.0, 0.0 );
    
    inputActuator->overrideActuation( state, true );
    feedbackActuator->overrideActuation( state, true );
  
    coordinate.setValue( state, 0.0 );
    
    manager = new OpenSim::Manager( *(model) );
    state.setTime( 0 );
    manager->initialize( state );
    
    initialTime = std::chrono::system_clock::now();
    simulationTime = std::chrono::system_clock::now();
  }
  
  ~OSimProcess()
  {
    model->setUseVisualizer( false );
    
    delete manager;
    delete model;
  }
  
  void Update()
  {
    const long MIN_TIME_STEP = 5;
    
    std::chrono::system_clock::time_point currentTime = std::chrono::system_clock::now();
    
    if( ( currentTime - simulationTime ).count() > MIN_TIME_STEP )
    {      
      simulationTime = currentTime;
      state = manager->integrate( ( simulationTime - initialTime ).count() / 1000.0 );
      std::cout << "running update loop" << std::endl;
    }
  }
  
  inline std::string GetName() { return model->getName(); }
  inline double GetPosition() { return inputActuator->getCoordinate()->getValue( state ); }
  inline double GetVelocity() { return inputActuator->getCoordinate()->getSpeedValue( state ); }
  inline double GetAcceleration() { return inputActuator->getCoordinate()->getAccelerationValue( state ); }
  inline double GetForce() { return inputActuator->getOverrideActuation( state ); }
  inline void SetForce( double value ) { feedbackActuator->setOverrideActuation( state, value ); }
  
private:
  OpenSim::Model* model; 
  OpenSim::Body* body;
  OpenSim::Manager* manager;
  OpenSim::CoordinateActuator* inputActuator;
  OpenSim::CoordinateActuator* feedbackActuator;
  SimTK::State state;
  
  std::chrono::system_clock::time_point initialTime, simulationTime;
};

DECLARE_MODULE_INTERFACE( SIGNAL_IO_INTERFACE );

int InitDevice( const char* modelName )
{  
  for( int deviceIndex = 0; deviceIndex < processList.size(); deviceIndex++ )
    if( processList[ deviceIndex ]->GetName() == modelName ) return deviceIndex;
    
  processList.push_back( new OSimProcess( modelName, BLOCK_COLORS[ processList.size() ] ) );
  std::cout << "device index: " << (int) processList.size() - 1 << std::endl;
  return (int) processList.size() - 1;
}

void EndDevice( int processID )
{
  for( OSimProcess* process : processList )
    delete process;
  
  return;
}

size_t GetMaxInputSamplesNumber( int processID )
{
  return 1;
}

size_t Read( int processID, unsigned int channel, double* ref_value )
{
  *ref_value = 0.0;
  
  if( processID < 0 ) return 0;
  
  OSimProcess* process = processList[ (size_t) processID ];
  
  if( channel > 3 ) return 0;
  
  process->Update();
  
  if( channel == 0 ) *ref_value = process->GetPosition();
  else if( channel == 1 ) *ref_value = process->GetVelocity();
  else if( channel == 2 ) *ref_value = process->GetAcceleration();
  else if( channel == 3 ) *ref_value = process->GetForce();
  
  return 1;
}

bool HasError( int processID )
{
  return false;
}

void Reset( int processID )
{
  return;
}

bool CheckInputChannel( int processID, unsigned int channel )
{
  if( processID < 0 ) return false;
  
  if( channel > 3 ) return false;
  
  return true;
}

bool Write( int processID, unsigned int channel, double value )
{
  if( processID < 0 ) return false;
  
  if( channel != 0 ) return false;
  
  OSimProcess* process = processList[ (size_t) processID ];
  
  process->Update();
  
  process->SetForce( value );
  
  return true;
}

bool AcquireOutputChannel( int taskID, unsigned int channel )
{
  return true;
}

void ReleaseOutputChannel( int taskID, unsigned int channel )
{
  return;
}
