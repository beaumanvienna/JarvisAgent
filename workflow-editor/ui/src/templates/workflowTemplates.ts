import type { JcwfFile } from "../jcwf/types";

export type WorkflowTemplate = {
  id: string;
  name: string;
  description: string;
  jcwf: JcwfFile;
};

export const workflowTemplates: WorkflowTemplate[] = [
  {
    id: "hello_world",
    name: "Hello World",
    description: "A simple workflow with a single shell task that prints a greeting.",
    jcwf: {
      version: "1.0",
      id: "hello_world",
      label: "Hello World",
      tasks: {
        greet: {
          id: "greet",
          type: "shell",
          label: "Print greeting",
          params: {
            command: "scripts/echo.sh",
            args: ["Hello, World!"],
          },
        },
      },
    },
  },
  {
    id: "sequential_pipeline",
    name: "Sequential Pipeline",
    description: "A three-step pipeline demonstrating task dependencies.",
    jcwf: {
      version: "1.0",
      id: "sequential_pipeline",
      label: "Sequential Pipeline",
      tasks: {
        step_1: {
          id: "step_1",
          type: "shell",
          label: "Step 1: Prepare",
          params: {
            command: "scripts/echo.sh",
            args: ["Preparing..."],
          },
        },
        step_2: {
          id: "step_2",
          type: "shell",
          label: "Step 2: Process",
          depends_on: ["step_1"],
          params: {
            command: "scripts/echo.sh",
            args: ["Processing..."],
          },
        },
        step_3: {
          id: "step_3",
          type: "shell",
          label: "Step 3: Finalize",
          depends_on: ["step_2"],
          params: {
            command: "scripts/echo.sh",
            args: ["Done!"],
          },
        },
      },
    },
  },
  {
    id: "parallel_tasks",
    name: "Parallel Tasks",
    description: "Multiple tasks running in parallel, then joining to a final step.",
    jcwf: {
      version: "1.0",
      id: "parallel_tasks",
      label: "Parallel Tasks",
      tasks: {
        start: {
          id: "start",
          type: "shell",
          label: "Start",
          params: {
            command: "scripts/echo.sh",
            args: ["Starting parallel workflow"],
          },
        },
        task_a: {
          id: "task_a",
          type: "shell",
          label: "Task A",
          depends_on: ["start"],
          params: {
            command: "scripts/echo.sh",
            args: ["Running Task A"],
          },
        },
        task_b: {
          id: "task_b",
          type: "shell",
          label: "Task B",
          depends_on: ["start"],
          params: {
            command: "scripts/echo.sh",
            args: ["Running Task B"],
          },
        },
        task_c: {
          id: "task_c",
          type: "shell",
          label: "Task C",
          depends_on: ["start"],
          params: {
            command: "scripts/echo.sh",
            args: ["Running Task C"],
          },
        },
        join: {
          id: "join",
          type: "shell",
          label: "Join Results",
          depends_on: ["task_a", "task_b", "task_c"],
          params: {
            command: "scripts/echo.sh",
            args: ["All parallel tasks complete"],
          },
        },
      },
    },
  },
  {
    id: "ai_call_example",
    name: "AI Call Example",
    description: "A workflow demonstrating an AI call task with prompt configuration.",
    jcwf: {
      version: "1.0",
      id: "ai_call_example",
      label: "AI Call Example",
      tasks: {
        ask_ai: {
          id: "ask_ai",
          type: "ai_call",
          label: "Ask AI",
          params: {
            prompt: "Summarize the key benefits of workflow automation in 3 bullet points.",
            model: "default",
          },
        },
      },
    },
  },
  {
    id: "python_script",
    name: "Python Script",
    description: "A workflow with a Python task for data processing.",
    jcwf: {
      version: "1.0",
      id: "python_script",
      label: "Python Script",
      tasks: {
        run_python: {
          id: "run_python",
          type: "python",
          label: "Run Python Script",
          params: {
            script: "print('Hello from Python!')\nresult = 2 + 2\nprint(f'2 + 2 = {result}')",
          },
        },
      },
    },
  },
];
