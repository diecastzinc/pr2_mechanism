/*
 * Copyright (c) 2008, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Author: Stuart Glaser, Wim Meeussen
 */

#include "pr2_mechanism_model/robot.h"
#include "pr2_mechanism_model/transmission.h"
#include <tinyxml/tinyxml.h>
#include <urdf/model.h>
#include <pluginlib/class_loader.h>
#include "pr2_hardware_interface/hardware_interface.h"


using namespace pr2_mechanism_model;
using namespace pr2_hardware_interface;


Robot::Robot(HardwareInterface *hw)
  :hw_(hw)
{}


bool Robot::initXml(TiXmlElement *root)
{
  // check if current time is valid
  if (!hw_){
    ROS_ERROR("Mechanism Model received an invalid hardware interface");
    return false;
  }

  // Parses the xml into a robot model
  if (!robot_model_.initXml(root)){
    ROS_ERROR("Mechanism Model failed to parse the URDF xml into a robot model");
    return false;
  }

  // Constructs the transmissions by parsing custom xml.
  pluginlib::ClassLoader<pr2_mechanism_model::Transmission> transmission_loader("pr2_mechanism_model", "pr2_mechanism_model::Transmission");
  TiXmlElement *xit = NULL;
  for (xit = root->FirstChildElement("transmission"); xit;
       xit = xit->NextSiblingElement("transmission"))
  {
    const char *type = xit->Attribute("type");
    Transmission *t;
    try{
      t = type ? transmission_loader.createClassInstance(type) : NULL;
    }
    catch(pluginlib::LibraryLoadException ex)
    {
      ROS_ERROR("LibraryLoadException for transmission of type %s", type);
      ROS_ERROR("%s", ex.what());
      return false;
    }
    catch(pluginlib::CreateClassException ex)
    {
      ROS_ERROR("CreateClassException for transmission of type %s", type);
      ROS_ERROR("%s", ex.what());
      return false;
    }
    catch(...)
    {
      ROS_ERROR("Could not construct transmission of type %s", type);
      return false;
    }

    if (!t)
      ROS_ERROR("Unknown transmission type: %s", type);
    else if (!t->initXml(xit, this)){
      ROS_ERROR("Failed to initialize transmission");
      delete t;
    }
    else // Success!
      transmissions_.push_back(t);
  }

  return true;
}

ros::Time Robot::getTime()
{
  return hw_->current_time_;
}

template <class T>
int findIndexByName(const std::vector<T*>& v, const std::string &name)
{
  for (unsigned int i = 0; i < v.size(); ++i)
  {
    if (v[i]->name_ == name)
      return i;
  }
  return -1;
}

int Robot::getTransmissionIndex(const std::string &name) const
{
  return findIndexByName(transmissions_, name);
}

Actuator* Robot::getActuator(const std::string &name) const
{
  return hw_->getActuator(name);
}

Transmission* Robot::getTransmission(const std::string &name) const
{
  int i = getTransmissionIndex(name);
  return i >= 0 ? transmissions_[i] : NULL;
}





RobotState::RobotState(Robot *model)
  : model_(model)
{
  assert(model_);

  transmissions_in_.resize(model->transmissions_.size());
  transmissions_out_.resize(model->transmissions_.size());

  // Creates a joint state for each transmission
  unsigned int js_size = 0;
  for (unsigned int i = 0; i < model_->transmissions_.size(); ++i)
  {
    Transmission *t = model_->transmissions_[i];
    for (unsigned int j = 0; j < t->actuator_names_.size(); ++j)
    {
      Actuator *act = model_->getActuator(t->actuator_names_[j]);
      assert(act != NULL);
      transmissions_in_[i].push_back(act);
    }
    js_size += t->joint_names_.size();
  }

  // Wires up the transmissions to the joint state
  joint_states_.resize(js_size);
  unsigned int js_id = 0;
  for (unsigned int i = 0; i < model_->transmissions_.size(); ++i)
  {
    Transmission *t = model_->transmissions_[i];
    for (unsigned int j = 0; j < t->joint_names_.size(); ++j)
    {
      joint_states_[js_id].joint_ = model_->robot_model_.getJoint(t->joint_names_[j]);
      joint_states_map_[t->joint_names_[j]] = &(joint_states_[js_id]);
      transmissions_out_[i].push_back(&(joint_states_[js_id]));
      js_id++;
    }
  }

  // warnings
  if (model_->transmissions_.empty())
    ROS_WARN("No transmissions were specified in the robot description.");
  if (js_size == 0)
    ROS_WARN("None of the joints in the robot desription matches up to a motor. The robot is uncontrollable.");
}


JointState *RobotState::getJointState(const std::string &name)
{
  std::map<std::string, JointState*>::iterator it = joint_states_map_.find(name);
  if (it == joint_states_map_.end())
    return NULL;
  else
    return it->second;
}

const JointState *RobotState::getJointState(const std::string &name) const
{
  std::map<std::string, JointState*>::const_iterator it = joint_states_map_.find(name);
  if (it == joint_states_map_.end())
    return NULL;
  else
    return it->second;
}

void RobotState::propagateActuatorPositionToJointPosition()
{
  for (unsigned int i = 0; i < model_->transmissions_.size(); ++i)
  {
    model_->transmissions_[i]->propagatePosition(transmissions_in_[i],
                                                 transmissions_out_[i]);
  }

  for (unsigned int i = 0; i < joint_states_.size(); i++)
  {
    joint_states_[i].joint_statistics_.update(&(joint_states_[i]));
  }
}

void RobotState::propagateJointEffortToActuatorEffort()
{
  for (unsigned int i = 0; i < model_->transmissions_.size(); ++i)
  {
    model_->transmissions_[i]->propagateEffort(transmissions_out_[i],
                                               transmissions_in_[i]);
  }
}

bool RobotState::isHalted()
{
  for (unsigned int t = 0; t < transmissions_in_.size(); ++t){
    for (unsigned int a = 0; a < transmissions_in_[t].size(); a++){
      if (transmissions_in_[t][a]->state_.halted_)
        return true;
    }
  }

  return false;
}

void RobotState::enforceSafety()
{
  for (unsigned int i = 0; i < joint_states_.size(); ++i)
  {
    joint_states_[i].enforceLimits();
  }
}

void RobotState::zeroCommands()
{
  for (unsigned int i = 0; i < joint_states_.size(); ++i)
    joint_states_[i].commanded_effort_ = 0;
}

void RobotState::propagateJointPositionToActuatorPosition()
{
  for (unsigned int i = 0; i < model_->transmissions_.size(); ++i)
  {
    model_->transmissions_[i]->propagatePositionBackwards(transmissions_out_[i],
                                                          transmissions_in_[i]);
  }
}

void RobotState::propagateActuatorEffortToJointEffort()
{
  for (unsigned int i = 0; i < model_->transmissions_.size(); ++i)
  {
    model_->transmissions_[i]->propagateEffortBackwards(transmissions_in_[i],
                                                        transmissions_out_[i]);
  }
}

