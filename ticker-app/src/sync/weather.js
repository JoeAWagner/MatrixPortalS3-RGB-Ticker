'use strict';

// Open-Meteo: free, no API key. ZIP -> coordinates -> current conditions.

const GEOCODE_URL = 'https://geocoding-api.open-meteo.com/v1/search';
const FORECAST_URL = 'https://api.open-meteo.com/v1/forecast';

// Icon ids must match iconBits[] in the firmware.
const ICON = { SUN: 0, CLOUD: 1, PARTLY: 2, RAIN: 3, SNOW: 4, STORM: 5, FOG: 6 };

// WMO code -> [short label that fits a 64px row, icon id]
const WMO = {
  0: ['Clear', ICON.SUN], 1: ['Clear', ICON.SUN],
  2: ['Cloudy', ICON.PARTLY], 3: ['Overcast', ICON.CLOUD],
  45: ['Fog', ICON.FOG], 48: ['Fog', ICON.FOG],
  51: ['Drizzle', ICON.RAIN], 53: ['Drizzle', ICON.RAIN], 55: ['Drizzle', ICON.RAIN],
  56: ['Freezing', ICON.SNOW], 57: ['Freezing', ICON.SNOW],
  61: ['Rain', ICON.RAIN], 63: ['Rain', ICON.RAIN], 65: ['Hvy Rain', ICON.RAIN],
  66: ['Icy Rain', ICON.SNOW], 67: ['Icy Rain', ICON.SNOW],
  71: ['Snow', ICON.SNOW], 73: ['Snow', ICON.SNOW], 75: ['Hvy Snow', ICON.SNOW],
  77: ['Snow', ICON.SNOW],
  80: ['Showers', ICON.RAIN], 81: ['Showers', ICON.RAIN], 82: ['Showers', ICON.RAIN],
  85: ['Snow', ICON.SNOW], 86: ['Snow', ICON.SNOW],
  95: ['Storm', ICON.STORM], 96: ['Storm', ICON.STORM], 99: ['Storm', ICON.STORM],
};

const geoCache = new Map();

async function geocode(zip) {
  if (geoCache.has(zip)) return geoCache.get(zip);
  const url = `${GEOCODE_URL}?name=${encodeURIComponent(zip)}&count=1&country=US`;
  const res = await fetch(url, { signal: AbortSignal.timeout(8000) });
  if (!res.ok) throw new Error(`geocoding HTTP ${res.status}`);
  const data = await res.json();
  if (!data.results || !data.results.length) throw new Error(`no location for "${zip}"`);
  const { latitude, longitude, name } = data.results[0];
  const hit = { lat: latitude, lon: longitude, name };
  geoCache.set(zip, hit);
  return hit;
}

/** Returns {text, icon, place} or throws. */
async function getWeather(zip, units = 'fahrenheit') {
  const { lat, lon, name } = await geocode(zip);
  const url = `${FORECAST_URL}?latitude=${lat}&longitude=${lon}`
    + `&current=temperature_2m,weather_code&temperature_unit=${units}&timezone=auto`;
  const res = await fetch(url, { signal: AbortSignal.timeout(8000) });
  if (!res.ok) throw new Error(`forecast HTTP ${res.status}`);
  const cur = (await res.json()).current;
  const temp = Math.round(cur.temperature_2m);
  const unit = units === 'fahrenheit' ? 'F' : 'C';
  const [label, icon] = WMO[cur.weather_code] || ['', -1];
  return { text: `${temp}${unit} ${label}`.trim(), icon, place: name };
}

module.exports = { getWeather, ICON, WMO };
