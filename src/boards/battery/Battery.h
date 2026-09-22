#pragma once

class Battery
{
public:
  virtual void setup() = 0;
  virtual float get_voltage() = 0;
  virtual int get_percentage() = 0;
  virtual bool is_charging() = 0;
  virtual bool get_low_power_state() = 0;
};