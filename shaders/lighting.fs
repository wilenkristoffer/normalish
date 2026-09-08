#version 330

// Inputs from raylib's default vertex shader.
in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;   // albedo: either the photo or a live webcam frame
uniform vec4 colDiffuse;

uniform vec2 texelSize;       // 1.0 / source texture size, for the Sobel taps
uniform float relief;         // how pronounced the derived relief is
uniform float sampleSpacing;  // tap distance in texels; larger ignores fine detail

uniform vec3 lightPos;   // surface units: x in 0..aspect, y in 0..1, z above surface
uniform vec3 lightColor;
uniform float lightIntensity;
uniform float falloff;
uniform float ambient;
uniform float specularStrength;
uniform float shininess;
uniform float aspect;
uniform int viewMode;    // 0 = lit, 1 = normals, 2 = albedo only

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

void main()
{
    vec3 albedo = texture(texture0, fragTexCoord).rgb*colDiffuse.rgb*fragColor.rgb;
    vec3 normal = computeNormal(fragTexCoord);

    // The surface is the z = 0 plane measured in units of its own height, so
    // the light stays circular on a non-square image.
    vec3 surfacePos = vec3(fragTexCoord.x*aspect, fragTexCoord.y, 0.0);
    vec3 toLight = lightPos - surfacePos;
    float dist = length(toLight);
    vec3 L = toLight/max(dist, 0.0001);
    vec3 V = vec3(0.0, 0.0, 1.0);   // orthographic viewer looking straight on
    vec3 H = normalize(L + V);

    float atten = lightIntensity/(1.0 + falloff*dist*dist);
    float diffuse = max(dot(normal, L), 0.0)*atten;
    float specular = pow(max(dot(normal, H), 0.0), shininess)*specularStrength*atten;

    vec3 color = albedo*(ambient + diffuse*lightColor) + specular*lightColor;

    if (viewMode == 1) color = normal*0.5 + 0.5;
    else if (viewMode == 2) color = albedo;

    finalColor = vec4(color, 1.0);
}
