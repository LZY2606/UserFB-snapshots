// Runnable entry point for the device-free seek example.
//
// The example logic itself lives in task_src/seek_example.cpp as
// `r8b_seek_example()`, so that the task build (which links every .cpp in
// task_src/) stays free of a second `main`.
//
// Build and run (from the repository root):
//
//   c++ -std=c++17 -O2 -I. task_src/seek_example.cpp examples/seek_example.cpp \
//       -o seek_example
//   ./seek_example

int r8b_seek_example();

int main()
{
	return( r8b_seek_example() );
}
