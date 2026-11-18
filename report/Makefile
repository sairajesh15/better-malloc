PDFLATEX ?= pdflatex

all: report.pdf srm_coa_report.pdf

report.pdf: report.tex
	@if which $(PDFLATEX) >/dev/null 2>&1; then \
		echo "Building report.pdf with pdflatex..."; \
		$(PDFLATEX) -interaction=nonstopmode report.tex && $(PDFLATEX) -interaction=nonstopmode report.tex; \
	else \
		echo "pdflatex not found on host. Building publication PDF with generate_report_pdf.py..."; \
		python3 generate_report_pdf.py; \
	fi

srm_coa_report.pdf: srm_coa_report.tex
	@if which $(PDFLATEX) >/dev/null 2>&1; then \
		echo "Building srm_coa_report.pdf with pdflatex..."; \
		$(PDFLATEX) -interaction=nonstopmode srm_coa_report.tex && $(PDFLATEX) -interaction=nonstopmode srm_coa_report.tex; \
	else \
		echo "pdflatex not found on host. To compile srm_coa_report.tex into PDF, use Overleaf or install MacTeX / pdflatex."; \
	fi

clean:
	rm -f *.aux *.log *.out *.toc report.pdf srm_coa_report.pdf

.PHONY: all clean
