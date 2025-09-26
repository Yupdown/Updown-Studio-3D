#pragma once

struct Vector2D
{
    float x;
    float y;

    Vector2D() : Vector2D(0.0f, 0.0f) {}
    Vector2D(float x, float y)
    {
        this->x = x;
        this->y = y;
    }

    Vector2D operator+() const
    {
        return Vector2D(x, y);
    }

    Vector2D operator-() const
    {
        return Vector2D(-x, -y);
    }

    Vector2D operator+(const Vector2D& other) const
    {
        return Vector2D(x + other.x, y + other.y);
    }

    Vector2D operator-(const Vector2D& other) const
    {
        return Vector2D(x - other.x, y - other.y);
    }

    Vector2D operator*(const float s) const
    {
        return Vector2D(x * s, y * s);
    }

    Vector2D operator/(const float s) const
    {
        return Vector2D(x / s, y / s);
    }

    Vector2D& operator+=(const Vector2D& other)
    {
        *this = *this + other;
        return *this;
    }

    Vector2D& operator-=(const Vector2D& other)
    {
        *this = *this - other;
        return *this;
    }

    Vector2D& operator*=(const float s)
    {
        *this = *this * s;
        return *this;
    }

    Vector2D& operator/=(const float s)
    {
        *this = *this / s;
        return *this;
    }
};