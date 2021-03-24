//
// Created by qiayuan on 12/28/20.
//
#include "rm_base/hardware_interface/can_bus.h"

#include <string>
#include <ros/ros.h>
#include <rm_common/math_utilities.h>
namespace rm_base {

float int16ToFloat(unsigned short data) {
  if (data == 0)
    return 0;
  float *fp32;
  unsigned int fInt32 = ((data & 0x8000) << 16) |
      (((((data >> 10) & 0x1f) - 0x0f + 0x7f) & 0xff) << 23) | ((data & 0x03FF) << 13);
  fp32 = (float *) &fInt32;
  return *fp32;
}

CanBus::CanBus(const std::string &bus_name, CanDataPtr data_prt)
    : data_prt_(data_prt), bus_name_(bus_name) {
  // Initialize device at can_device, false for no loop back.
  while (!socket_can_.open(bus_name, [this](auto &&PH1) { frameCallback(std::forward<decltype(PH1)>(PH1)); })
      && ros::ok())
    ros::Duration(.5).sleep();

  ROS_INFO("Successfully connected to %s.", bus_name.c_str());
  // Set up CAN package header
  rm_frame0_.can_id = 0x200;
  rm_frame0_.can_dlc = 8;
  rm_frame1_.can_id = 0x1FF;
  rm_frame1_.can_dlc = 8;
}

void CanBus::write() {
  bool has_write_frame0{}, has_write_frame1{};
  // safety first
  std::fill(std::begin(rm_frame0_.data), std::end(rm_frame0_.data), 0);
  std::fill(std::begin(rm_frame1_.data), std::end(rm_frame1_.data), 0);

  for (auto &item:*data_prt_.id2act_data_) {
    if (item.second.type.find("rm") != std::string::npos) {
      if (item.second.temp > 100) // check temperature
        continue;
      const ActCoeff &act_coeff = data_prt_.type2act_coeffs_->find(item.second.type)->second;
      int id = item.first - 0x201;
      double cmd = minAbs(act_coeff.effort2act * item.second.cmd_effort, act_coeff.max_out); //add max_range to act_data
      if (-1 < id && id < 4) {
        rm_frame0_.data[2 * id] = (uint8_t) (static_cast<int16_t>(cmd) >> 8u);
        rm_frame0_.data[2 * id + 1] = (uint8_t) cmd;
        has_write_frame0 = true;
      } else if (3 < id && id < 8) {
        rm_frame1_.data[2 * (id - 4)] = (uint8_t) (static_cast<int16_t>(cmd) >> 8u);
        rm_frame1_.data[2 * (id - 4) + 1] = (uint8_t) cmd;
        has_write_frame1 = true;
      }
    } else if (item.second.type.find("cheetah") != std::string::npos) {
      can_frame frame{};
      const ActCoeff &act_coeff = data_prt_.type2act_coeffs_->find(item.second.type)->second;
      frame.can_id = item.first;
      frame.can_dlc = 8;
      uint16_t q_des = (int) (act_coeff.pos2act * (item.second.cmd_pos - act_coeff.act2pos_offset));
      uint16_t qd_des = (int) (act_coeff.vel2act * (item.second.cmd_vel - act_coeff.act2vel_offset));
      uint16_t kp = 0.;
      uint16_t kd = 0.;
      uint16_t tau = (int) (act_coeff.effort2act * (item.second.cmd_effort - act_coeff.act2effort_offset));
      // TODO(qiayuan) add posistion vel and effort hardware interface for MIT Cheetah Motor.
      frame.data[0] = q_des >> 8;
      frame.data[1] = q_des & 0xFF;
      frame.data[2] = qd_des >> 4;
      frame.data[3] = ((qd_des & 0xF) << 4) | (kp >> 8);
      frame.data[4] = kp & 0xFF;
      frame.data[5] = kd >> 4;
      frame.data[6] = ((kd & 0xF) << 4) | (tau >> 8);
      frame.data[7] = tau & 0xff;
      socket_can_.wirte(&frame);
    }
  }

  if (has_write_frame0)
    socket_can_.wirte(&rm_frame0_);
  if (has_write_frame1)
    socket_can_.wirte(&rm_frame1_);
}

void CanBus::frameCallback(const can_frame &frame) {
  // Check if robomaster motor
  if (data_prt_.id2act_data_->find(frame.can_id) != data_prt_.id2act_data_->end()) {
    ActData &act_data = data_prt_.id2act_data_->find(frame.can_id)->second;
    const ActCoeff &act_coeff = data_prt_.type2act_coeffs_->find(act_data.type)->second;

    if (act_data.type.find("rm") != std::string::npos) {
      uint16_t q = (frame.data[0] << 8u) | frame.data[1];
      int16_t qd = (frame.data[2] << 8u) | frame.data[3];
      int16_t cur = (frame.data[4] << 8u) | frame.data[5];
      uint8_t temp = frame.data[6];

      // Multiple cycle
      if (act_data.seq != 0) {
        if (q - act_data.q_last > 4096)
          act_data.q_circle--;
        else if (q - act_data.q_last < -4096)
          act_data.q_circle++;
      }
      act_data.seq++;
      act_data.q_last = q;
      // Converter raw CAN data to position velocity and effort.
      act_data.pos = act_coeff.act2pos * static_cast<double> (q + 8191 * act_data.q_circle);
      act_data.vel = act_coeff.act2vel * static_cast<double> (qd);
      act_data.effort = act_coeff.act2effort * static_cast<double> (cur);
      act_data.temp = temp;
      // Low pass filt
      act_data.lp_filter->input(act_data.vel);
      act_data.vel = act_data.lp_filter->output();
      return;
    }
  }
    // Check MIT Cheetah motor
  else if (frame.can_id == static_cast<unsigned int>(0x000)) {
    if (data_prt_.id2act_data_->find(frame.data[0]) != data_prt_.id2act_data_->end()) {
      ActData &act_data = data_prt_.id2act_data_->find(frame.data[0])->second;
      const ActCoeff &act_coeff = data_prt_.type2act_coeffs_->find(act_data.type)->second;
      if (act_data.type.find("cheetah") != std::string::npos) { // MIT Cheetah Motor
        uint16_t q = (frame.data[1] << 8) | frame.data[2];
        uint16_t qd = (frame.data[3] << 4) | (frame.data[4] >> 4);
        uint16_t cur = ((frame.data[4] & 0xF) << 8) | frame.data[5];
        // Converter raw CAN data to position velocity and effort.
        act_data.vel = act_coeff.act2vel * static_cast<double> (qd) + act_coeff.act2vel_offset;
        act_data.effort = act_coeff.act2effort * static_cast<double> (cur) + act_coeff.act2effort_offset;
        // Multiple cycle
        // NOTE: Raw data range is -4pi~4pi
        if (act_data.seq != 0) {
          double pos_new =
              act_coeff.act2pos * static_cast<double> (q) + act_coeff.act2pos_offset
                  + static_cast<double>(act_data.q_circle) * 8 * M_PI;
          if (pos_new - act_data.pos > 4 * M_PI)
            act_data.q_circle--;
          else if (pos_new - act_data.pos < -4 * M_PI)
            act_data.q_circle++;
        }
        act_data.seq++;
        act_data.pos = act_coeff.act2pos * static_cast<double> (q) + act_coeff.act2pos_offset
            + static_cast<double>(act_data.q_circle) * 8 * M_PI;
        // Low pass filt
        act_data.lp_filter->input(act_data.vel);
        act_data.vel = act_data.lp_filter->output();
      }
    }
  }
  // Check if IMU
  float imu_frame_data[4] = {0};
  bool is_too_big = false; // int16ToFloat failed sometime
  for (int i = 0; i < 4; ++i) {
    float value = int16ToFloat((frame.data[i * 2] << 8) | frame.data[i * 2 + 1]);
    if (value > 1e3 || value < -1e3)
      is_too_big = true;
    else
      imu_frame_data[i] = value;
  }
  if (!is_too_big) {
    for (auto &itr :*data_prt_.id2imu_data_) { // imu data are consisted of three frames
      switch (frame.can_id - static_cast<unsigned int>(itr.first)) {
        case 0:itr.second.linear_acc[0] = imu_frame_data[0] * 9.81;
          itr.second.linear_acc[1] = imu_frame_data[1] * 9.81;
          itr.second.linear_acc[2] = imu_frame_data[2] * 9.81;
          itr.second.angular_vel[0] = imu_frame_data[3] / 360. * 2. * M_PI;
          return;
        case 1:itr.second.angular_vel[1] = imu_frame_data[0] / 360. * 2. * M_PI;
          itr.second.angular_vel[2] = imu_frame_data[1] / 360. * 2. * M_PI;
          itr.second.ori[3] = imu_frame_data[2]; // Note the quaternion order
          itr.second.ori[0] = imu_frame_data[3];
          return;
        case 2:itr.second.ori[1] = imu_frame_data[0];
          itr.second.ori[2] = imu_frame_data[1];
          return;
        default:break;
      }
    }
  }
  if (!is_too_big)
    ROS_ERROR_STREAM_ONCE(
        "Can not find defined device, id: 0x" << std::hex << frame.can_id << " on bus: " << bus_name_);
}

}
