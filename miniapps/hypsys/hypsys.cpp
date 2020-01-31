#include "lib/fe_evol.hpp"

int main(int argc, char *argv[])
{
   Configuration config;
   config.ProblemNum = 0;
   config.ConfigNum = 1;
   const char *MeshFile = "data/unstr.mesh";
   int refinements = 1;
   config.order = 3;
   config.tFinal = 1.;
   config.dt = 0.001;
   config.odeSolverType = 3;
   config.VisSteps = 100;

   EvolutionScheme scheme = Standard;

   config.precision = 8;
   cout.precision(config.precision);

   OptionsParser args(argc, argv);
   args.AddOption(&config.ProblemNum, "-p", "--problem",
                  "Hyperbolic system of equations to solve.");
   args.AddOption(&config.ConfigNum, "-c", "--configuration",
                  "Problem setup to use.");
   args.AddOption(&MeshFile, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&refinements, "-r", "--refine",
                  "Number of times to refine the mesh uniformly.");
   args.AddOption(&config.order, "-o", "--order",
                  "Order (polynomial degree) of the finite element space.");
   args.AddOption(&config.tFinal, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&config.dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&config.odeSolverType, "-s", "--ode-solver",
                  "ODE solver: 1 - Forward Euler,\n\t"
                  "            2 - RK2 SSP, 3 - RK3 SSP.");
   args.AddOption(&config.VisSteps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.AddOption((int*)(&scheme), "-e", "--EvolutionScheme",
                  "Scheme: 0 - Standard Finite Element Approximation,\n\t"
                  "        1 - Monolithic Convex Limiting.");

   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return -1;
   }
   args.PrintOptions(cout);

   ODESolver *odeSolver = NULL;
   switch (config.odeSolverType)
   {
      case 1: odeSolver = new ForwardEulerSolver; break;
      case 2: odeSolver = new RK2Solver(1.0); break;
      case 3: odeSolver = new RK3SSPSolver; break;
      default:
         cout << "Unknown ODE solver type: " << config.odeSolverType << endl;
         return -1;
   }

   Mesh mesh(MeshFile, 1, 1);
   const int dim = mesh.Dimension();

   for (int lev = 0; lev < refinements; lev++)
   {
      mesh.UniformRefinement();
   }
   if (mesh.NURBSext)
   {
      mesh.SetCurvature(max(config.order, 1));
   }

   mesh.GetBoundingBox(config.bbMin, config.bbMax, max(config.order, 1));

   // Create Bernstein Finite Element Space.
   const int btype = BasisType::Positive;
   L2_FECollection fec(config.order, dim, btype);
   FiniteElementSpace fes(&mesh, &fec);

   const int ProblemSize = fes.GetVSize();
   cout << "Number of unknowns: " << ProblemSize << endl;

   // The min/max bounds are represented as H1 functions of the same order
   // as the solution, thus having 1:1 dof correspondence inside each element.
   H1_FECollection fecBounds(max(config.order, 1), dim,
                             BasisType::GaussLobatto);
   FiniteElementSpace fesBounds(&mesh, &fecBounds);
   DofInfo dofs(&fes, &fesBounds);

   // Compute the lumped mass matrix.
   Vector LumpedMassMat;
   BilinearForm ml(&fes);
   ml.AddDomainIntegrator(new LumpedIntegrator(new MassIntegrator));
   ml.Assemble();
   ml.Finalize();
   ml.SpMat().GetDiag(LumpedMassMat);

   HyperbolicSystem *hyp;
   switch (config.ProblemNum)
   {
      case 0: { hyp =  new Advection(&fes, config); break; }
      default:
         cout << "Unknown hyperbolic system: " << config.ProblemNum << '\n';
         return -1;
   }

   if (config.odeSolverType != 1 && hyp->SteadyState)
   {
      MFEM_WARNING("You should use forward Euler for pseudo time stepping.");
   }

   GridFunction u(&fes);
   u = hyp->u0;

   double InitialMass = LumpedMassMat * u;

   // Visualization with GLVis, VisIt is currently not supported.
   if (hyp->FileOutput)
   {
      ofstream omesh("grid.mesh");
      omesh.precision(config.precision);
      mesh.Print(omesh);
      ofstream osol("initial.gf");
      osol.precision(config.precision);
      u.Save(osol);
   }

   socketstream sout;
   char vishost[] = "localhost";
   int  visport   = 19916;
   bool VectorOutput = false; // TODO
   {
      VisualizeField(sout, vishost, visport, u, VectorOutput);
   }

   FE_Evolution evol(&fes, hyp, dofs, scheme, LumpedMassMat);

   odeSolver->Init(evol);
   if (hyp->SteadyState)
   {
      evol.uOld.SetSize(ProblemSize);
      evol.uOld = 0.;
   }

   bool done = false;
   double dt, res, t = 0., tol = 1.e-12;
   for (int ti = 0; !done;)
   {
      dt = min(config.dt, config.tFinal - t);
      odeSolver->Step(u, t, dt);
      ti++;

      done = (t >= config.tFinal - 1.e-8*config.dt);

      if (hyp->SteadyState)
      {
         res = evol.ConvergenceCheck(dt, tol, u);
         if (res < tol)
         {
            done = true;
            u = evol.uOld;
         }
      }

      if (done || ti % config.VisSteps == 0)
      {
         if (hyp->SteadyState)
         {
            cout << "time step: " << ti << ", time: " << t <<
                 ", residual: " << res << endl;
         }
         else
         {
            cout << "time step: " << ti << ", time: " << t << endl;
         }
         VisualizeField(sout, vishost, visport, u, VectorOutput);
      }
   }

   double DomainSize = LumpedMassMat.Sum();
   cout << "Difference in solution mass: "
        << abs(InitialMass - LumpedMassMat * u) / DomainSize << endl;

   if (hyp->SolutionKnown && hyp->FileOutput)
   {
      Array<double> errors;
      hyp->ComputeErrors(errors, DomainSize, u);
      hyp->WriteErrors(errors);
   }

   if (hyp->FileOutput)
   {
      ofstream osol("final.gf");
      osol.precision(config.precision);
      u.Save(osol);
   }

   delete hyp;
   delete odeSolver;
   return 0;
}