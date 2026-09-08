#version 330

// Inputs from raylib's default vertex shader.
in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;   // albedo: either the photo or a live webcam frame
uniform vec4 colDiffuse;

uniform vec2 texelSize;       // 1.0 / source texture size, for the Sobel taps
uniform float relief;         // how pronounced the derived relief is
uniform float sampleSpacing;  // tap distance in texels; larger ignores fine detail

// Geometry model: 0 = luminance relief (flat plane, orthographic viewer, the
// original), 1 = a real depth map with a perspective camera. The two use
// different spaces, and lightPos is supplied in whichever one is active.
uniform int geometryMode;

uniform sampler2D depthMap;   // R channel: metres along the view axis, 0 = no data
uniform float tanHalfFovX;    // tan of half the horizontal field of view
uniform float depthSpacing;   // tap distance in texels for the depth gradient
uniform vec2 depthRange;      // near/far, only for the depth view mode

uniform vec3 lightPos;   // luminance mode: x in 0..aspect, y in 0..1, z above
                         // surface. depth mode: metres in camera space.
uniform vec3 lightColor;
uniform float lightIntensity;
uniform float falloff;
uniform float ambient;
uniform float specularStrength;
uniform float shininess;
uniform float aspect;
uniform int viewMode;    // 0 = lit, 1 = normals, 2 = albedo only, 3 = depth

out vec4 finalColor;

// Luminance as height: the same approximation the CPU generator used, but
// evaluated per pixel on the GPU so it can keep up with live video.
float heightAt(vec2 uv)
{
    vec3 color = texture(texture0, uv).rgb;
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Sobel filter over the luminance, then encode the slopes as a normal.
// Widening sampleSpacing takes the place of the CPU blur: bilinear filtering
// averages as the taps spread out, so fine noise stops dominating the gradient.
vec3 computeNormal(vec2 uv)
{
    vec2 d = texelSize*sampleSpacing;

    float tl = heightAt(uv + vec2(-d.x, -d.y));
    float t  = heightAt(uv + vec2( 0.0, -d.y));
    float tr = heightAt(uv + vec2( d.x, -d.y));
    float l  = heightAt(uv + vec2(-d.x,  0.0));
    float r  = heightAt(uv + vec2( d.x,  0.0));
    float bl = heightAt(uv + vec2(-d.x,  d.y));
    float b  = heightAt(uv + vec2( 0.0,  d.y));
    float br = heightAt(uv + vec2( d.x,  d.y));

    float gx = ((tr + 2.0*r + br) - (tl + 2.0*l + bl))/8.0;
    float gy = ((bl + 2.0*b + br) - (tl + 2.0*t + tr))/8.0;

    return normalize(vec3(-gx*relief, -gy*relief, 1.0));
}

// --- depth model -----------------------------------------------------------
//
// The camera sits at the origin looking down +z, so depth grows away from the
// viewer and a surface facing the camera has a normal pointing at -z. Image
// space has y = 0 at the top, so +y points down in 3D as well.

float depthAt(vec2 uv)
{
    return texture(depthMap, uv).r;
}

// A depth sample back to the 3D point that produced it.
vec3 depthPosition(vec2 uv)
{
    float z = depthAt(uv);
    vec2 ndc = uv*2.0 - 1.0;
    return vec3(ndc.x*tanHalfFovX*z, ndc.y*(tanHalfFovX/aspect)*z, z);
}

// One axis of the surface gradient, as the better of the two one-sided
// differences. Across a silhouette the far neighbour is metres away and would
// dominate a central difference, tilting the normal into nonsense; taking
// whichever side is nearer in depth keeps the estimate on the surface we are
// actually standing on. Invalid neighbours are pushed out of contention.
vec3 axisDelta(vec2 uv, vec3 P, float z, vec2 step)
{
    float zb = depthAt(uv - step);
    float zf = depthAt(uv + step);

    float db = (zb > 0.0) ? abs(z - zb) : 1.0e9;
    float df = (zf > 0.0) ? abs(zf - z) : 1.0e9;
    if ((db > 1.0e8) && (df > 1.0e8)) return vec3(0.0);

    return (df <= db) ? (depthPosition(uv + step) - P)
                      : (P - depthPosition(uv - step));
}

vec3 normalFromDepth(vec2 uv)
{
    float z = depthAt(uv);
    if (z <= 0.0) return vec3(0.0, 0.0, -1.0);

    vec2 d = texelSize*max(depthSpacing, 1.0);
    vec3 P = depthPosition(uv);

    vec3 Px = axisDelta(uv, P, z, vec2(d.x, 0.0));
    vec3 Py = axisDelta(uv, P, z, vec2(0.0, d.y));

    vec3 n = cross(Py, Px);
    float len = length(n);
    if (len <= 1.0e-12) return vec3(0.0, 0.0, -1.0);
    return n/len;
}

void main()
{
    vec3 albedo = texture(texture0, fragTexCoord).rgb*colDiffuse.rgb*fragColor.rgb;

    vec3 P;        // surface position, in whichever space the mode uses
    vec3 normal;
    vec3 V;        // surface to viewer

    if (geometryMode == 1)
    {
        P = depthPosition(fragTexCoord);
        normal = normalFromDepth(fragTexCoord);
        // A real perspective viewer, so the specular highlight moves with
        // position rather than sitting dead ahead everywhere.
        V = (P.z > 0.0) ? normalize(-P) : vec3(0.0, 0.0, -1.0);
    }
    else
    {
        // The surface is the z = 0 plane measured in units of its own height,
        // so the light stays circular on a non-square image.
        P = vec3(fragTexCoord.x*aspect, fragTexCoord.y, 0.0);
        normal = computeNormal(fragTexCoord);
        V = vec3(0.0, 0.0, 1.0);   // orthographic viewer looking straight on
    }

    vec3 toLight = lightPos - P;
    float dist = length(toLight);
    vec3 L = toLight/max(dist, 0.0001);
    vec3 H = normalize(L + V);

    float atten = lightIntensity/(1.0 + falloff*dist*dist);
    float diffuse = max(dot(normal, L), 0.0)*atten;
    float specular = pow(max(dot(normal, H), 0.0), shininess)*specularStrength*atten;

    vec3 color = albedo*(ambient + diffuse*lightColor) + specular*lightColor;

    if (viewMode == 1) color = normal*0.5 + 0.5;
    else if (viewMode == 2) color = albedo;
    else if (viewMode == 3)
    {
        // Near is white, far is black, no data is red so it cannot be
        // mistaken for geometry.
        float z = depthAt(fragTexCoord);
        if (z <= 0.0) color = vec3(0.6, 0.0, 0.0);
        else
        {
            float t = (z - depthRange.x)/max(depthRange.y - depthRange.x, 0.0001);
            color = vec3(1.0 - clamp(t, 0.0, 1.0));
        }
    }

    finalColor = vec4(color, 1.0);
}
