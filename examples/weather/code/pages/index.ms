// The forecast for a place, from the US National Weather Service's API
// (https://www.weather.gov/documentation/services-web-api). It needs no
// key: it's two plain GETs, the second to a URL the first one returns.

defaultLat = "39.7456"
defaultLon = "-97.0892"

// The first few forecast periods for lat, lon; or an error value.
forecast = function(lat, lon)
    headers = {"Accept": "application/geo+json"}
    points = http.get("https://api.weather.gov/points/" + lat + "," + lon, headers)
    if points isa error then return points
    if points.status != 200 then return err("the weather service doesn't know that place (" + points.status + ")")
    // A URL from a response is untrusted input, like anything a visitor
    // types: the engine checks it against outbound.ms like any other.
    resp = http.get(points.json.properties.forecast, headers)
    if resp isa error then return resp
    if resp.status != 200 then return err("no forecast just now (" + resp.status + ")")
    return resp.json.properties.periods[:4]
end function

render = function(req)
    lat = defaultLat
    lon = defaultLon
    if req.query.hasIndex("lat") then lat = req.query.lat
    if req.query.hasIndex("lon") then lon = req.query.lon
    fields = {"lat": lat, "lon": lon, "periods": null, "problem": null}
    periods = forecast(lat, lon)
    if periods isa error then
        fields.problem = periods.message
    else
        rows = []
        for p in periods
            rows.push {"name": p.name, "temp": p.temperature, "unit": p.temperatureUnit, "text": p.shortForecast}
        end for
        fields.periods = frag("period", rows)
    end if
    return fields
end function
