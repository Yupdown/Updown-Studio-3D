#pragma once

struct Vector3D
{
	float x;
	float y;
	float z;

    Vector3D() : Vector3D(0.0f, 0.0f, 0.0f) {}
    Vector3D(float x, float y, float z)
    {
        this->x = x;
        this->y = y;
        this->z = z;
    }

    Vector3D operator+() const
    {
        return Vector3D(x, y, z);
    }

    Vector3D operator-() const
    {
        return Vector3D(-x, -y, -z);
    }

    Vector3D operator+(const Vector3D& other) const
    {
        return Vector3D(x + other.x, y + other.y, z + other.z);
    }

    Vector3D operator-(const Vector3D& other) const
    {
        return Vector3D(x - other.x, y - other.y, z - other.z);
    }

    Vector3D operator*(const float s) const
    {
        return Vector3D(x * s, y * s, z * s);
    }

    Vector3D operator/(const float s) const
    {
        return Vector3D(x / s, y / s, z / s);
    }

    Vector3D& operator+=(const Vector3D& other)
    {
        *this = *this + other;
        return *this;
    }

    Vector3D& operator-=(const Vector3D& other)
    {
        *this = *this - other;
        return *this;
    }

    Vector3D& operator*=(const float s)
    {
        *this = *this * s;
        return *this;
    }
    
    Vector3D& operator/=(const float s)
    {
        *this = *this / s;
        return *this;
    }
};