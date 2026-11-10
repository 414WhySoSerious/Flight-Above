# Flight-Above
This is a 100% Copilot generated code project. I got the initial environment setup for the ESP32 with the cheap yellow display (CYD), enough to blink an LED, and then had Copilot go at it. Copilot started from a Python script that Claude created to do proof of concept that the information was freely accessible over the internet.

Route data comes from a callsign lookup and may be stale when a flight number is reused. The display checks the reported route against the aircraft's live position and shows "route unverified" instead of an implausible route.
