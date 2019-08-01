
#include <iostream>

#include "../eph_spline.h"
#include "../eph_beta.h"

#if 0
int main() {
  std::vector<double> y = {10, 9, 8, 7, 6};
  double dx = 1; 
  
  EPH_Spline<> spl(dx, y);
  //EPH_Spline<> spl;
  
  //std::cout << spl(0) << " " << spl(1) << " " << spl(2) << " " << spl(3) 
  //  << " " << spl(4) << " " << spl(1.5) << '\n';
  
  //std::cout << spl(4.1) << '\n';
  
  for(size_t i = 0; i < 100; ++i) {
    double x = 0.05 * i;
    std::cout << x << " " << spl(x) << '\n';
  }
  return 0;
}
#endif

#if 1
int main() {
  EPH_Beta<> beta;
  beta = EPH_Beta<>("Beta_Rho.beta");
  
  double r_cutoff = beta.get_r_cutoff();
  double dr = 0.001;
  size_t N = r_cutoff / dr;
  
  std::cout << "# " << beta.get_n_elements() << 
    ' ' << beta.get_r_cutoff() << ' ' << beta.get_rho_cutoff() << '\n';
  
  for(size_t i = 0; i < beta.get_n_elements(); ++i) {
    std::cout << "# " << beta.get_element_name(i) << ' ' << beta.get_element_number(i) << '\n';
  }
  
  for(size_t i = 0; i < N; ++i) {
    double r = i * dr;
    
    std::cout << r << " " << beta.get_rho(0, r) << '\n';
  }
  
  return 0;
}


#endif

