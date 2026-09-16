int signbit(double x)
{
	union { double d; unsigned long long u; } bits;
	bits.d = x;
	return (int)(bits.u >> 63);
}
