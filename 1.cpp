#include <iostream>
#include <string>
#include <stdio.h>

using namespace std;

int main() {
	string s;
	s = "abacaba";
	s[3] = char(0);
	cout << s.size() << endl;
	cout << s << endl;
}
