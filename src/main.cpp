#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>
#include "wifi_credentials.h"

#define LED_PIN 4

constexpr float LATITUDE = 43.0389f;
constexpr float LONGITUDE = -87.9065f;
constexpr int SEARCH_RADIUS_MILES = 20;
constexpr int SEARCH_RADIUS_NM = 17;
constexpr unsigned long POLL_INTERVAL_MS = 10000;
constexpr unsigned long PROVIDER_COOLDOWN_MS = 600000;
constexpr char USER_AGENT[] = "Mozilla/5.0 (compatible; overhead-flights-prototype/0.1)";
constexpr char ROUTE_URL[] = "https://api.adsbdb.com/v0/callsign/";
constexpr char TZ_INFO[] = "CST6CDT,M3.2.0/2,M11.1.0/2";

struct Aircraft {
  String hex;
  String callsign;
  String planeType;
  String altitudeText;
  float altitudeFeet = 0;
  float speedKnots = 0;
  float distanceNm = 999;
  bool onGround = false;
  bool hasAltitude = false;
  bool hasSpeed = false;
  bool hasDistance = false;
};

struct Route {
  String airline;
  String flightNumber;
  String origin;
  String destination;
};

struct RouteCacheEntry {
  String callsign;
  bool hasRoute = false;
  Route route;
};

struct Provider {
  const char* name;
  const char* urlPrefix;
  unsigned long blockedUntil = 0;
};

enum class JsonResult {
  Ok,
  NotFound,
  Error
};

Provider providers[] = {
    {"adsb.fi", "https://opendata.adsb.fi/api/v2/lat/"},
    {"adsb.lol", "https://api.adsb.lol/v2/point/"},
};

std::vector<RouteCacheEntry> routeCache;
std::vector<String> announced;
bool wasConnected = false;
bool timeWasSynchronized = false;
unsigned long lastPollAt = 0;
unsigned long lastTimeSyncMessageAt = 0;
bool firstPoll = true;

bool containsString(const std::vector<String>& values, const String& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool isAirlineCallsign(String& callsign) {
  callsign.trim();
  callsign.toUpperCase();

  const size_t length = callsign.length();
  if (length < 4 || length > 9) {
    return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    if (callsign[i] < 'A' || callsign[i] > 'Z') {
      return false;
    }
  }

  size_t index = 3;
  size_t digitCount = 0;
  while (index < length && callsign[index] >= '0' && callsign[index] <= '9') {
    ++index;
    ++digitCount;
  }
  if (digitCount == 0 || digitCount > 4) {
    return false;
  }

  size_t suffixLength = 0;
  while (index < length && callsign[index] >= 'A' && callsign[index] <= 'Z') {
    ++index;
    ++suffixLength;
  }
  return index == length && suffixLength <= 2;
}

const char* airlineName(const String& code) {
  struct Airline {
    const char* code;
    const char* name;
  };
  static const Airline airlines[] = {
      {"AAL", "American Airlines"}, {"UAL", "United Airlines"},
      {"DAL", "Delta Air Lines"},   {"SWA", "Southwest Airlines"},
      {"ASA", "Alaska Airlines"},   {"JBU", "JetBlue"},
      {"NKS", "Spirit Airlines"},   {"FFT", "Frontier Airlines"},
      {"AAY", "Allegiant Air"},     {"SCX", "Sun Country"},
      {"HAL", "Hawaiian Airlines"}, {"SKW", "SkyWest Airlines"},
      {"RPA", "Republic Airways"},  {"ENY", "Envoy Air"},
      {"EDV", "Endeavor Air"},      {"JIA", "PSA Airlines"},
      {"GJS", "GoJet Airlines"},    {"CPZ", "Compass Airlines"},
      {"QXE", "Horizon Air"},       {"FDX", "FedEx"},
      {"UPS", "UPS Airlines"},      {"ACA", "Air Canada"},
      {"WJA", "WestJet"},           {"BAW", "British Airways"},
      {"DLH", "Lufthansa"},         {"AFR", "Air France"},
      {"KLM", "KLM"},               {"UAE", "Emirates"},
      {"VIR", "Virgin Atlantic"},
  };
  for (const Airline& airline : airlines) {
    if (code == airline.code) {
      return airline.name;
    }
  }
  return nullptr;
}

void addAircraftFilter(JsonDocument& filter, const char* arrayName) {
  filter[arrayName][0]["hex"] = true;
  filter[arrayName][0]["flight"] = true;
  filter[arrayName][0]["t"] = true;
  filter[arrayName][0]["alt_baro"] = true;
  filter[arrayName][0]["gs"] = true;
  filter[arrayName][0]["dst"] = true;
}

class HttpBodyStream : public Stream {
 public:
  HttpBodyStream(NetworkClient& client, bool chunked, int contentLength)
      : client_(client),
        chunked_(chunked),
        contentRemaining_(contentLength) {
    setTimeout(20000);
  }

  int available() override {
    if (peeked_ >= 0) {
      return 1;
    }
    if (finished_) {
      return 0;
    }
    return client_.available();
  }

  int read() override {
    if (peeked_ >= 0) {
      const int value = peeked_;
      peeked_ = -1;
      return value;
    }
    if (finished_) {
      return -1;
    }

    if (chunked_) {
      if (!advanceChunk()) {
        return -1;
      }
    } else if (contentRemaining_ == 0) {
      finished_ = true;
      return -1;
    }

    const int value = readClientByte();
    if (value < 0) {
      finished_ = true;
      return -1;
    }
    if (chunked_) {
      --chunkRemaining_;
    } else if (contentRemaining_ > 0) {
      --contentRemaining_;
    }
    return value;
  }

  int peek() override {
    if (peeked_ < 0) {
      peeked_ = read();
    }
    return peeked_;
  }

  size_t write(uint8_t) override {
    return 0;
  }

 private:
  int readClientByte() {
    const unsigned long startedAt = millis();
    while (client_.available() <= 0) {
      if (!client_.connected() || millis() - startedAt >= getTimeout()) {
        return -1;
      }
      delay(1);
    }
    return client_.read();
  }

  bool readLine(String& line) {
    line = "";
    while (line.length() < 128) {
      const int value = readClientByte();
      if (value < 0) {
        return false;
      }
      if (value == '\n') {
        line.trim();
        return true;
      }
      line += static_cast<char>(value);
    }
    return false;
  }

  bool consumeChunkTerminator() {
    const int carriageReturn = readClientByte();
    const int lineFeed = readClientByte();
    return carriageReturn == '\r' && lineFeed == '\n';
  }

  bool advanceChunk() {
    if (chunkRemaining_ > 0) {
      return true;
    }
    if (finished_) {
      return false;
    }

    if (!firstChunk_ && !consumeChunkTerminator()) {
      finished_ = true;
      return false;
    }
    firstChunk_ = false;

    String line;
    if (!readLine(line)) {
      finished_ = true;
      return false;
    }
    const int extension = line.indexOf(';');
    if (extension >= 0) {
      line.remove(extension);
    }
    char* end = nullptr;
    const unsigned long chunkSize = strtoul(line.c_str(), &end, 16);
    if (end == line.c_str() || *end != '\0') {
      finished_ = true;
      return false;
    }
    if (chunkSize == 0) {
      do {
        if (!readLine(line)) {
          finished_ = true;
          return false;
        }
      } while (!line.isEmpty());
      finished_ = true;
      return false;
    }
    chunkRemaining_ = chunkSize;
    return true;
  }

  NetworkClient& client_;
  bool chunked_;
  int contentRemaining_;
  unsigned long chunkRemaining_ = 0;
  bool firstChunk_ = true;
  bool finished_ = false;
  int peeked_ = -1;
};

JsonResult getJson(const String& url, JsonDocument& document,
                   bool filterAircraft = false) {
  WiFiClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();

  HTTPClient http;
  if (!http.begin(secureClient, url)) {
    Serial.printf("  [error] Could not start request: %s\n", url.c_str());
    return JsonResult::Error;
  }
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(USER_AGENT);
  http.addHeader("Accept", "application/json");
  const char* responseHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(responseHeaders, 1);

  const int status = http.GET();
  if (status == HTTP_CODE_NOT_FOUND) {
    http.end();
    return JsonResult::NotFound;
  }
  if (status < 0 || status < 200 || status >= 300) {
    Serial.printf("  [http %d] %s\n", status, url.c_str());
    http.end();
    return JsonResult::Error;
  }

  document.clear();
  const int responseSize = http.getSize();
  const bool chunked =
      http.header("Transfer-Encoding").indexOf("chunked") >= 0;
  HttpBodyStream body(http.getStream(), chunked, responseSize);
  body.setTimeout(20000);
  DeserializationError error;
  if (filterAircraft) {
    JsonDocument filter;
    addAircraftFilter(filter, "ac");
    addAircraftFilter(filter, "aircraft");
    error = deserializeJson(document, body,
                            DeserializationOption::Filter(filter));
  } else {
    error = deserializeJson(document, body);
  }
  http.end();
  if (error) {
    Serial.printf("  [json error] %s: %s (response size: %d bytes)\n",
                  url.c_str(), error.c_str(), responseSize);
    return JsonResult::Error;
  }
  return JsonResult::Ok;
}

bool isProviderBlocked(const Provider& provider, unsigned long now) {
  return provider.blockedUntil != 0 &&
         static_cast<int32_t>(provider.blockedUntil - now) > 0;
}

String buildPositionUrl(const Provider& provider) {
  String url(provider.urlPrefix);
  if (strcmp(provider.name, "adsb.fi") == 0) {
    url += String(LATITUDE, 4) + "/lon/" + String(LONGITUDE, 4) + "/dist/";
  } else {
    url += String(LATITUDE, 4) + "/" + String(LONGITUDE, 4) + "/";
  }
  url += String(SEARCH_RADIUS_NM);
  return url;
}

std::vector<Aircraft> getNearbyAircraft() {
  const unsigned long now = millis();
  for (Provider& provider : providers) {
    if (isProviderBlocked(provider, now)) {
      continue;
    }

    JsonDocument document;
    const String url = buildPositionUrl(provider);
    const JsonResult result = getJson(url, document, true);
    JsonVariantConst aircraftValue = document["ac"];
    if (result == JsonResult::Ok &&
        (!aircraftValue.is<JsonArrayConst>() ||
         aircraftValue.as<JsonArrayConst>().size() == 0)) {
      aircraftValue = document["aircraft"];
    }

    if (result == JsonResult::Ok && aircraftValue.is<JsonArrayConst>()) {
      std::vector<Aircraft> aircraft;
      for (JsonVariantConst item : aircraftValue.as<JsonArrayConst>()) {
        if (!item.is<JsonObjectConst>()) {
          continue;
        }

        Aircraft plane;
        plane.hex = item["hex"] | "";
        plane.callsign = item["flight"] | "";
        plane.planeType = item["t"] | "";
        plane.onGround = item["alt_baro"] == "ground";
        if (!plane.onGround && item["alt_baro"].is<float>()) {
          plane.altitudeFeet = item["alt_baro"].as<float>();
          plane.hasAltitude = true;
        }
        if (item["gs"].is<float>()) {
          plane.speedKnots = item["gs"].as<float>();
          plane.hasSpeed = true;
        }
        if (item["dst"].is<float>()) {
          plane.distanceNm = item["dst"].as<float>();
          plane.hasDistance = true;
        }
        aircraft.push_back(plane);
      }
      return aircraft;
    }

    provider.blockedUntil = millis() + PROVIDER_COOLDOWN_MS;
    Serial.printf("  [%s] failed; skipping it for %lu min\n", provider.name,
                  PROVIDER_COOLDOWN_MS / 60000);
  }
  return {};
}

String formatAirport(JsonVariantConst airport) {
  if (airport.isNull()) {
    return "?";
  }
  String city = airport["municipality"] | "";
  if (city.isEmpty()) {
    city = airport["name"] | "";
  }
  if (city.isEmpty()) {
    city = "?";
  }

  String code = airport["iata_code"] | "";
  if (code.isEmpty()) {
    code = airport["icao_code"] | "";
  }
  return code.isEmpty() ? city : city + " (" + code + ")";
}

bool lookupRoute(const String& callsign, Route& route) {
  for (const RouteCacheEntry& entry : routeCache) {
    if (entry.callsign == callsign) {
      if (entry.hasRoute) {
        route = entry.route;
        return true;
      }
      return false;
    }
  }

  JsonDocument document;
  const JsonResult result = getJson(String(ROUTE_URL) + callsign, document);
  delay(500);
  if (result == JsonResult::Error) {
    return false;
  }

  JsonVariantConst flightRoute = document["response"]["flightroute"];
  RouteCacheEntry entry;
  entry.callsign = callsign;
  if (flightRoute.is<JsonObjectConst>()) {
    entry.route.airline = flightRoute["airline"]["name"] | "";
    entry.route.flightNumber = flightRoute["callsign_iata"] | "";
    entry.route.origin = formatAirport(flightRoute["origin"]);
    entry.route.destination = formatAirport(flightRoute["destination"]);
    entry.hasRoute = true;
    route = entry.route;
  }
  routeCache.push_back(entry);
  return entry.hasRoute;
}

bool describeAircraft(Aircraft& aircraft, String& description) {
  if (!isAirlineCallsign(aircraft.callsign) || aircraft.onGround) {
    return false;
  }

  Route route;
  const bool hasRoute = lookupRoute(aircraft.callsign, route);
  const String code = aircraft.callsign.substring(0, 3);
  const char* fallbackName = airlineName(code);
  const String airline = hasRoute && !route.airline.isEmpty()
                             ? route.airline
                             : (fallbackName != nullptr ? fallbackName : code);
  const String flightNumber =
      hasRoute && !route.flightNumber.isEmpty() ? route.flightNumber : aircraft.callsign;
  const String trip = hasRoute
                          ? route.origin + " -> " + route.destination
                          : "route unknown";
  const String altitude = aircraft.hasAltitude && aircraft.altitudeFeet != 0
                              ? String(static_cast<long>(aircraft.altitudeFeet)) + " ft"
                              : "alt ?";
  const String speed = aircraft.hasSpeed && aircraft.speedKnots != 0
                           ? String(static_cast<long>(roundf(aircraft.speedKnots))) + " kt"
                           : "speed ?";
  const String distance = aircraft.hasDistance
                              ? String(aircraft.distanceNm, 1) + " nm away"
                              : "";

  description = airline + " " + flightNumber + " | " + trip + " | " +
                aircraft.planeType + " " + altitude + ", " + speed + ", " + distance;
  return true;
}

void pollAircraft() {
  std::vector<Aircraft> aircraft = getNearbyAircraft();
  std::sort(aircraft.begin(), aircraft.end(),
            [](const Aircraft& left, const Aircraft& right) {
              return left.distanceNm < right.distanceNm;
            });

  std::vector<String> current;
  current.reserve(aircraft.size());
  size_t newlyAnnounced = 0;
  for (Aircraft& plane : aircraft) {
    current.push_back(plane.hex);
    if (containsString(announced, plane.hex)) {
      continue;
    }

    String description;
    if (describeAircraft(plane, description)) {
      time_t now = time(nullptr);
      struct tm localTime;
      char timestamp[9] = "??:??:??";
      if (localtime_r(&now, &localTime) != nullptr) {
        strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &localTime);
      }
      Serial.printf("%s %s\n", timestamp, description.c_str());
      announced.push_back(plane.hex);
      ++newlyAnnounced;
    }
  }

  announced.erase(
      std::remove_if(announced.begin(), announced.end(),
                     [&current](const String& hex) {
                       return !containsString(current, hex);
                     }),
      announced.end());
  Serial.printf("Poll complete: %u aircraft returned; %u new airline flights announced.\n",
                static_cast<unsigned>(aircraft.size()),
                static_cast<unsigned>(newlyAnnounced));
}

bool clockIsSynchronized() {
  return time(nullptr) > 1700000000;
}

void setup() {
  pinMode(LED_PIN, OUTPUT);

  Serial.begin(115200);
  Serial.println("Flight Above starting");
  Serial.printf("Watching within %d miles (%d nm) of %.4f, %.4f.\n",
                SEARCH_RADIUS_MILES, SEARCH_RADIUS_NM, LATITUDE, LONGITUDE);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting to Wi-Fi...");
}

void loop() {
  const unsigned long now = millis();
  const bool isConnected = WiFi.status() == WL_CONNECTED;

  if (isConnected && !wasConnected) {
    Serial.print("Connected to Wi-Fi. IP address: ");
    Serial.println(WiFi.localIP());
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    timeWasSynchronized = false;
    lastTimeSyncMessageAt = now;
    lastPollAt = now;
    firstPoll = true;
  } else if (!isConnected && wasConnected) {
    Serial.println("Wi-Fi disconnected.");
    timeWasSynchronized = false;
  }
  wasConnected = isConnected;

  if (isConnected && !timeWasSynchronized && clockIsSynchronized()) {
    Serial.println("Clock synchronized; HTTPS certificate checks are ready.");
    timeWasSynchronized = true;
    firstPoll = true;
  } else if (isConnected && !timeWasSynchronized &&
             now - lastTimeSyncMessageAt >= 30000) {
    Serial.println("Waiting for network time before making verified HTTPS requests.");
    lastTimeSyncMessageAt = now;
  }

  if (isConnected && timeWasSynchronized &&
      (firstPoll || now - lastPollAt >= POLL_INTERVAL_MS)) {
    pollAircraft();
    lastPollAt = millis();
    firstPoll = false;
  }

  static unsigned long lastLedToggleAt = 0;
  static bool ledIsOn = false;
  if (now - lastLedToggleAt >= 1000) {
    ledIsOn = !ledIsOn;
    digitalWrite(LED_PIN, ledIsOn ? HIGH : LOW);
    lastLedToggleAt = now;
  }
  delay(10);
}
