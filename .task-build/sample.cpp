void observe(const int*);
bool ready(const int*);
void f(int* p) { if (ready(p)) { *p = 1; } }
